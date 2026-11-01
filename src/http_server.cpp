#include "http_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace dcache {

namespace {

constexpr std::size_t kMaxRequestSize = 64 * 1024;

// 单机允许同时挂起的连接上限。每个未完成的连接要占 64KB 读缓冲，
// 不设上限时「开一堆连接只发半个请求」就能把节点内存吃干净。
constexpr std::size_t kMaxConns = 8192;

// 空闲连接回收阈值与扫描周期。
constexpr int kIdleTimeoutMs  = 10000;
constexpr int kSweepIntervalMs = 1000;

// 阻塞写响应的最长等待时间。没有它，一个「只发请求不读响应」的客户端
// 就能让 send() 永久阻塞，几个连接即可占满全部 worker（廉价 DoS）。
constexpr int kSendTimeoutMs = 2000;

// 解析请求；返回 1=完整，0=还需更多数据，-1=报文非法，-2=声明体过大。
int parse_request(const std::string& data, HttpRequest& req) {
    const std::size_t hpos = data.find("\r\n\r\n");
    if (hpos == std::string::npos) {
        return data.size() > kMaxRequestSize ? -1 : 0;
    }

    const std::string header_block = data.substr(0, hpos);
    const std::size_t line_end = header_block.find("\r\n");
    const std::string reqline = header_block.substr(0, line_end);

    // 请求行："GET /api/g/k HTTP/1.1"
    std::size_t sp1 = reqline.find(' ');
    std::size_t sp2 = reqline.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        return -1;
    }
    req.method = reqline.substr(0, sp1);
    req.path   = reqline.substr(sp1 + 1, sp2 - sp1 - 1);

    // 头部行（可无）。
    req.headers.clear();
    std::size_t pos = line_end + 2;
    while (pos < hpos) {
        std::size_t line_end2 = header_block.find("\r\n", pos);
        if (line_end2 == std::string::npos) line_end2 = hpos;
        const std::string line = header_block.substr(pos, line_end2 - pos);
        if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                if (!v.empty() && v[0] == ' ') v.erase(0, 1);
                req.headers[k] = v;
            }
        }
        pos = line_end2 + 2;
    }

    // 请求体（Content-Length）。本项目只有 GET，body 通常为空。
    long content_length = 0;
    auto it = req.headers.find("Content-Length");
    if (it == req.headers.end()) it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        content_length = std::strtol(it->second.c_str(), nullptr, 10);
        if (content_length < 0) {
            return -1; // 非法（负数）Content-Length
        }
    }

    // 声明体超过上限时立刻判失败：读缓冲本来就在 kMaxRequestSize 处封顶，
    // 这种请求永远等不到「收全」，只会把这个连接永久挂在 conns_ 里。
    if (static_cast<std::size_t>(content_length) > kMaxRequestSize) {
        return -2;
    }

    const std::size_t total = hpos + 4 + static_cast<std::size_t>(content_length);
    if (data.size() < total) {
        return 0; // body 未收完
    }
    req.body = data.substr(hpos + 4, static_cast<std::size_t>(content_length));
    return 1;
}

std::string build_response(const HttpResponse& resp) {
    std::string s = "HTTP/1.1 " + std::to_string(resp.status) + " " +
                    HttpStatusText(resp.status) + "\r\n";

    std::string content_type = "text/plain";
    auto it = resp.headers.find("Content-Type");
    if (it != resp.headers.end()) {
        content_type = it->second;
    }
    s += "Content-Type: " + content_type + "\r\n";
    s += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    s += "Connection: close\r\n";

    for (const auto& h : resp.headers) {
        if (h.first == "Content-Type" || h.first == "Content-Length" ||
            h.first == "Connection") {
            continue; // 避免重复
        }
        s += h.first + ": " + h.second + "\r\n";
    }
    s += "\r\n";
    s += resp.body;
    return s;
}

bool set_nonblocking(int fd, bool on) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (on) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

bool set_send_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

bool write_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(const std::string& addr, std::uint16_t port, Handler handler,
                       int worker_count) {
    if (listen_fd_ >= 0) {
        return false; // 已启动
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (addr == "0.0.0.0") {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, addr.c_str(), &sin.sin_addr) != 1) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, SOMAXCONN) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // 取实际端口（port==0 时由内核分配）。
    socklen_t slen = sizeof(sin);
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&sin), &slen) == 0) {
        port_ = ntohs(sin.sin_port);
    } else {
        port_ = port;
    }

    if (!set_nonblocking(listen_fd_, true)) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) != 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    handler_ = std::move(handler);
    stop_ = false;

    // 预留一个 fd，供 EMFILE 时自救（见 run()）。取不到不影响启动，只是少一层保护。
    reserve_fd_ = ::open("/dev/null", O_RDONLY);

    // 复位关停标志与残留任务：Stop() 之后再 Start() 时，如果不复位 shutdown_，
    // 新建的工作线程会在第一次 wait 时立刻返回，此后所有请求进队列都无人消费。
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        shutdown_ = false;
        queue_.clear();
    }

    thread_ = std::thread(&HttpServer::run, this);

    if (worker_count < 1) {
        worker_count = 1;
    }
    workers_.reserve(static_cast<std::size_t>(worker_count));
    for (int i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&HttpServer::worker_loop, this);
    }
    return true;
}

void HttpServer::Stop() {
    stop_ = true;

    // ① 先停生产者：事件循环退出后就不会再往队列里塞任务。
    //    原顺序是「先置 shutdown_ 再 join 事件循环」，工作线程会因 shutdown_ 立刻返回，
    //    而事件循环在 epoll_wait 的超时窗口内仍可能入队——这些任务永远无人消费，
    //    fd 不关闭、客户端挂死。
    if (thread_.joinable()) {
        thread_.join();
    }

    // ② 再停消费者：置关停标志，让工作线程把队列排空后退出。
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        shutdown_ = true;
    }
    queue_cv_.notify_all();

    for (auto& w : workers_) {
        w.join();
    }
    workers_.clear();

    // ③ 兜底回收：正常路径下队列已被排空，异常路径下也避免残留任务泄漏 fd。
    std::deque<Task> leftover;
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        leftover.swap(queue_);
    }
    for (auto& t : leftover) {
        ::close(t.fd);
    }
}

void HttpServer::run() {
    constexpr int kMaxEvents = 64;
    struct epoll_event events[kMaxEvents];

    auto last_sweep = std::chrono::steady_clock::now();

    while (!stop_) {
        int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            std::uint32_t ev = events[i].events;

            if (fd == listen_fd_) {
                for (;;) {
                    struct sockaddr_in ca;
                    socklen_t calen = sizeof(ca);
                    int cfd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&ca), &calen);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // backlog 已排空
                        }
                        if (errno == EMFILE || errno == ENFILE) {
                            // fd 耗尽。此时不能简单 break：listen fd 在水平触发模式下
                            // 始终可读，epoll_wait 会立刻返回，事件循环变成 100% CPU 空转
                            // （最经典的 epoll 自旋 bug）。
                            //
                            // 而且此时 accept 本身就会失败，连「取走一个待处理连接」都做不到，
                            // 所以必须先释放预留 fd 腾出名额，把连接取出来关掉，再把名额占回去。
                            if (reserve_fd_ >= 0) {
                                ::close(reserve_fd_);
                                reserve_fd_ = -1;
                                int drop = ::accept(listen_fd_, nullptr, nullptr);
                                if (drop >= 0) {
                                    ::close(drop);
                                }
                                reserve_fd_ = ::open("/dev/null", O_RDONLY);
                            }
                            if (reserve_fd_ < 0) {
                                break; // 连预留 fd 都拿不回来，只能等下一轮
                            }
                            continue;
                        }
                        break; // ECONNABORTED 等，跳过这一个连接
                    }

                    if (conns_.size() >= kMaxConns) {
                        // 超过连接上限：不做无谓的排队，立刻拒绝，
                        // 否则内存会被「开连接不发请求」的客户端拖垮。
                        ::close(cfd);
                        continue;
                    }

                    set_nonblocking(cfd, true);
                    struct epoll_event cev;
                    std::memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN;
                    cev.data.fd = cfd;
                    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &cev) == 0) {
                        // 记录对端 IP：后续据此判断该请求是否来自集群节点，
                        // 决定能否信任 X-DCache-Hops 这类内部语义头。
                        Conn conn;
                        char ip[INET_ADDRSTRLEN] = {0};
                        if (::inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof(ip)) != nullptr) {
                            conn.remote_ip = ip;
                        }
                        conn.last_active = std::chrono::steady_clock::now();
                        conns_[cfd] = std::move(conn);
                    } else {
                        ::close(cfd);
                    }
                }
            } else if (ev & EPOLLIN) {
                // 必须先处理 EPOLLIN 再处理 HUP/ERR：
                // 「发完请求立刻 close」的客户端会同时带回 EPOLLIN|EPOLLHUP，
                // 若先走 HUP 分支直接关连接，缓冲区里那条完整请求就被丢掉了。
                handle_read(fd);
            } else if (ev & (EPOLLHUP | EPOLLERR)) {
                close_conn(fd);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sweep).count() >=
            kSweepIntervalMs) {
            sweep_idle();
            last_sweep = now;
        }
    }

    cleanup();
}

void HttpServer::handle_read(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return; // 已移交给工作线程，忽略同批残留事件
    }
    it->second.last_active = std::chrono::steady_clock::now();
    std::string& buf = it->second.read_buf;

    char tmp[8192];
    bool eof = false;
    for (;;) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            if (buf.size() > kMaxRequestSize) {
                close_conn(fd); // 报文超限，无需再读
                return;
            }
        } else if (n == 0) {
            eof = true;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            eof = true;
            break;
        }
    }

    HttpRequest req;
    int rc = parse_request(buf, req);
    if (rc == 0 && !eof) {
        return; // 等待更多数据（空闲超时由 sweep_idle 兜底回收）
    }

    if (rc == 1) {
        // 完整请求：从 epoll/conns_ 摘除，fd 所有权转移给工作线程。
        Task t;
        t.fd  = fd;
        t.req = std::move(req);
        t.req.remote_ip = it->second.remote_ip;  // 必须在 conns_.erase 之前取
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        conns_.erase(fd);
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            queue_.push_back(std::move(t));
        }
        queue_cv_.notify_one();
        return;
    }

    // 报文非法（-1）、声明体过大（-2）或连接已关闭但请求不完整：直接断开。
    // 这里不做 400/413 回包——在事件循环线程上做阻塞写会拖慢整节点，
    // 价值不如立刻释放资源。
    close_conn(fd);
}

void HttpServer::handle_request(int fd, HttpRequest req) {
    HttpResponse resp;
    resp.status = 200;

    // 异常绝不能逃出工作线程：逃出线程函数会触发 std::terminate，整个节点进程直接退出。
    // handler 会一路调到用户提供的 getter，抛异常是完全可能的正常情况。
    try {
        handler_(req, resp);
    } catch (...) {
        resp = HttpResponse{};
        resp.status = 500;
        resp.headers["Content-Type"] = "application/json";
        resp.body   = "{\"error\":\"internal\"}";
    }

    const std::string data = build_response(resp);

    // 阻塞写 + 发送超时：慢客户端最多占用 worker kSendTimeoutMs，之后放弃。
    set_nonblocking(fd, false);
    set_send_timeout(fd, kSendTimeoutMs);
    write_all(fd, data);  // 返回 false 表示对端已关闭或写超时，属于正常结束
    ::close(fd);
}

void HttpServer::worker_loop() {
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(queue_mtx_);
            // 有任务处理或收到关闭信号才唤醒（不能只等 shutdown_，否则新任务永远无人消费）。
            queue_cv_.wait(lk, [&] { return shutdown_ || !queue_.empty(); });
            if (shutdown_ && queue_.empty()) {
                return; // 关闭信号且队列已排空
            }
            t = std::move(queue_.front());
            queue_.pop_front();
        }
        handle_request(t.fd, std::move(t.req));
    }
}

void HttpServer::close_conn(int fd) {
    // 幂等：不在 conns_ 里说明 fd 已移交给工作线程，由工作线程负责关闭。
    // 这样即使同一批事件里残留了该 fd 的 HUP，也不会误关别人复用的同号 fd。
    if (conns_.erase(fd) == 0) {
        return;
    }
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
}

void HttpServer::sweep_idle() {
    const auto now = std::chrono::steady_clock::now();

    // 先收集再关闭：erase 会让迭代器失效。
    std::vector<int> victims;
    for (const auto& kv : conns_) {
        const auto idle_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - kv.second.last_active)
                .count();
        if (idle_ms >= kIdleTimeoutMs) {
            victims.push_back(kv.first);
        }
    }
    for (int fd : victims) {
        close_conn(fd);
    }
}

void HttpServer::cleanup() {
    for (auto& kv : conns_) {
        ::close(kv.first);
    }
    conns_.clear();
    if (reserve_fd_ >= 0) {
        ::close(reserve_fd_);
        reserve_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

} // namespace dcache