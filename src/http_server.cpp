#include "http_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "http_parser.hpp"

namespace dcache {

namespace {

// 单机允许同时挂起的连接上限。每个未完成的连接要占 64KB 读缓冲，
// 不设上限时「开一堆连接只发半个请求」就能把节点内存吃干净。
constexpr std::size_t kMaxConns = 8192;

// 空闲连接回收阈值与扫描周期。
constexpr int kIdleTimeoutMs   = 10000;
constexpr int kSweepIntervalMs = 1000;

// 写回一条响应最多允许停滞多久。没有它，一个「只发请求不读响应」的客户端
// 就能让这条连接永远挂在那里（连带那个 8MB 的响应缓冲）。
//
// 这个数原来是 SO_SNDTIMEO，含义是「工作线程被 send() 按住的时长」；
// 现在写由事件循环做，它变成「连接上多久没有写出进展就回收」。
// 数值不动是刻意的：公开行为（客户端能拖多久）保持不变，变的只是代价。
constexpr int kSendTimeoutMs = 2000;

// 一次 flush 最多推进多少字节就把事件循环让出去。
//
// 写本身是 memcpy，很快；但一条 8MB 的响应一口气推完，这一轮里其它连接的
// 响应就会被推迟（事件循环是单线程的）。给个上限，让大响应和小响应在同一轮
// 里公平分摊：多花的只是一次 epoll_wait 往返，换来的是延迟不随别人的响应体
// 大小抖动。之所以能给上限，正是因为 EPOLLOUT 是水平触发的——
// 注册着且没写完，下一轮一定会再进来。
constexpr std::size_t kMaxBytesPerFlush = 256 * 1024;

// 取头部值并做大小写不敏感比较（HTTP 头名与 Connection 的值都可能有大小写差异）。
bool header_equals_ci(const HttpRequest& req, const char* name, const char* value) {
    auto it = req.headers.find(name);
    if (it == req.headers.end()) {
        return false;
    }
    const std::string& actual = it->second;
    const std::size_t n       = std::strlen(value);
    if (actual.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(actual[i]);
        const unsigned char b = static_cast<unsigned char>(value[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

// 客户端是否显式要求复用连接。
bool wants_keep_alive(const HttpRequest& req) {
    return header_equals_ci(req, "Connection", "keep-alive");
}

std::string build_response(const HttpResponse& resp, bool keep_alive) {
    std::string s =
        "HTTP/1.1 " + std::to_string(resp.status) + " " + HttpStatusText(resp.status) + "\r\n";

    std::string content_type = "text/plain";
    auto it                  = resp.headers.find("Content-Type");
    if (it != resp.headers.end()) {
        content_type = it->second;
    }
    s += "Content-Type: " + content_type + "\r\n";
    s += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    s += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";

    for (const auto& h : resp.headers) {
        if (h.first == "Content-Type" || h.first == "Content-Length" || h.first == "Connection") {
            continue;  // 避免重复
        }
        s += h.first + ": " + h.second + "\r\n";
    }
    s += "\r\n";
    s += resp.body;
    return s;
}

bool set_nonblocking(int fd, bool on) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    if (on)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

}  // namespace

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(const std::string& addr, std::uint16_t port, Handler handler,
                       int worker_count, const tls::ServerConfig& tls_cfg) {
    if (listen_fd_ >= 0) {
        return false;  // 已启动
    }

    // TLS 上下文必须在监听之前建好：证书/私钥有问题时应该在启动阶段失败，
    // 而不是让服务跑起来、每个握手都失败、运维从 handshake_failures 里猜原因。
    if (!tls_cfg.cert_file.empty() || !tls_cfg.key_file.empty()) {
        std::string err;
        tls_ctx_ = tls::Context::CreateServer(tls_cfg, err);
        if (!tls_ctx_) {
            // 不用 fprintf：它是可变形参，clang-tidy 的
            // cppcoreguidelines-pro-type-vararg 会直接拦下。写 stderr 的效果一样。
            const std::string msg = "[http] TLS 初始化失败: " + err + "\n";
            const ssize_t n       = ::write(STDERR_FILENO, msg.data(), msg.size());
            (void)n;
            return false;
        }
        // 留着配置：ReloadTls 要重新读同一组文件。
        tls_cfg_     = tls_cfg;
        tls_enabled_ = true;
    } else {
        tls_ctx_.reset();
        tls_cfg_     = tls::ServerConfig{};
        tls_enabled_ = false;
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
    sin.sin_port   = htons(port);
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
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) != 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    handler_ = std::move(handler);
    stop_    = false;

    // 唤醒用的 eventfd：keep-alive 连接被交还时用它立刻叫醒 epoll。
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ >= 0) {
        struct epoll_event wev;
        std::memset(&wev, 0, sizeof(wev));
        wev.events  = EPOLLIN;
        wev.data.fd = wake_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &wev) != 0) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
    }
    // 拿不到 eventfd 不视为启动失败：退回「靠 epoll_wait 超时兜底」的模式，
    // 功能仍然正确，只是复用连接可能要多等一个超时周期。

    // 复位指标：同一个对象 Stop 后再 Start，计数应该从零开始，
    // 否则「重启后的服务」会带着上一轮的命中率，读数没有意义。
    requests_total_.store(0, std::memory_order_relaxed);
    requests_dropped_.store(0, std::memory_order_relaxed);
    responses_2xx_.store(0, std::memory_order_relaxed);
    responses_4xx_.store(0, std::memory_order_relaxed);
    responses_5xx_.store(0, std::memory_order_relaxed);
    active_conns_.store(0, std::memory_order_relaxed);
    handshake_failures_.store(0, std::memory_order_relaxed);
    tls_reload_total_.store(0, std::memory_order_relaxed);
    tls_reload_failures_.store(0, std::memory_order_relaxed);

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

HttpServer::Metrics HttpServer::GetMetrics() const {
    Metrics m;
    m.requests_total      = requests_total_.load(std::memory_order_relaxed);
    m.requests_dropped    = requests_dropped_.load(std::memory_order_relaxed);
    m.responses_2xx       = responses_2xx_.load(std::memory_order_relaxed);
    m.responses_4xx       = responses_4xx_.load(std::memory_order_relaxed);
    m.responses_5xx       = responses_5xx_.load(std::memory_order_relaxed);
    m.active_conns        = active_conns_.load(std::memory_order_relaxed);
    m.handshake_failures  = handshake_failures_.load(std::memory_order_relaxed);
    m.tls_enabled         = tls_enabled_;
    m.tls_reload_total    = tls_reload_total_.load(std::memory_order_relaxed);
    m.tls_reload_failures = tls_reload_failures_.load(std::memory_order_relaxed);
    return m;
}

bool HttpServer::ReloadTls(std::string& err) {
    if (!tls_enabled_) {
        err = "未启用 TLS：明文监听的端口没有证书可加载";
        return false;
    }

    // 先建后换。新上下文完全可用之后才替换 tls_ctx_ 指向的对象，
    // 中途任何一步失败时 tls_ctx_ 一个字节都没动过——这是「reload 失败不影响
    // 服务」的实现方式，而不是靠失败分支里去修复已经改坏的状态。
    std::unique_ptr<tls::Context> fresh = tls::Context::CreateServer(tls_cfg_, err);
    if (!fresh) {
        tls_reload_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 旧上下文的释放时机交给 OpenSSL 的引用计数：SSL_new 时为 SSL_CTX 加了一次
    // 引用，所以这里换掉 shared_ptr 只是减掉我们自己那一份，在途会话各自持有的
    // 那一份仍然有效，连接不会被掐断。等最后一个会话关闭，旧上下文才真正释放。
    std::atomic_store(&tls_ctx_, std::shared_ptr<tls::Context>(std::move(fresh)));
    tls_reload_total_.fetch_add(1, std::memory_order_relaxed);
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
    std::deque<Completed> leftover_conns;
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        leftover.swap(queue_);
        // 已写好但还没写出去的响应同理：Stop 之后事件循环已经退出，
        // 没有人再 drain 它们。工作线程若恰好在 Stop 之后才交还，
        // 这些 fd 只能在这里关闭。
        leftover_conns.swap(completed_);
    }
    for (auto& t : leftover) {
        t.ssl.reset();  // 先释放会话再关 fd（SSL_free 不做 I/O，顺序只是防日后改动踩坑）
        ::close(t.fd);
    }
    for (auto& r : leftover_conns) {
        r.ssl.reset();
        ::close(r.fd);
    }

    // wake_fd_ 必须等到工作线程全部退出后才关闭：queue_completed 在工作线程里写它，
    // 若在 cleanup() 里提前 close，worker 可能写出一个「已经被系统回收、又被别人占用」
    // 的 fd 号——那不是 fp 泄漏级别的问题，是往别人的连接里写脏数据。
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}

void HttpServer::run() {
    constexpr int kMaxEvents = 64;
    struct epoll_event events[kMaxEvents];

    auto last_sweep = std::chrono::steady_clock::now();

    while (!stop_) {
        int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, 100);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd           = events[i].data.fd;
            std::uint32_t ev = events[i].events;

            if (fd == wake_fd_) {
                // 把计数读空，否则水平触发模式下会反复唤醒（又变成空转）。
                std::uint64_t drained = 0;
                while (::read(wake_fd_, &drained, sizeof(drained)) > 0) {
                }
                drain_completed();
                continue;
            }

            if (fd == listen_fd_) {
                for (;;) {
                    struct sockaddr_in ca;
                    socklen_t calen = sizeof(ca);
                    int cfd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&ca), &calen);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // backlog 已排空
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
                                int drop    = ::accept(listen_fd_, nullptr, nullptr);
                                if (drop >= 0) {
                                    ::close(drop);
                                }
                                reserve_fd_ = ::open("/dev/null", O_RDONLY);
                            }
                            if (reserve_fd_ < 0) {
                                break;  // 连预留 fd 都拿不回来，只能等下一轮
                            }
                            continue;
                        }
                        break;  // ECONNABORTED 等，跳过这一个连接
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
                    cev.events  = EPOLLIN;
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

                        if (tls_enabled_) {
                            // 先取一份局部副本再建会话：reload 可能刚好在这两行之间
                            // 换掉 tls_ctx_，局部 shared_ptr 保证我们用的是自洽的那份，
                            // 而不是另一个线程刚减完引用、已经悬垂的上下文。
                            const std::shared_ptr<tls::Context> ctx = std::atomic_load(&tls_ctx_);
                            conn.ssl = ctx ? tls::Session::Accept(*ctx, cfd) : nullptr;
                            if (!conn.ssl) {
                                // SSL_new 失败基本只有「内存耗尽」一种可能，
                                // 但无论如何都不能把一条没有会话的连接留在表里。
                                handshake_failures_.fetch_add(1, std::memory_order_relaxed);
                                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cfd, nullptr);
                                ::close(cfd);
                                continue;
                            }
                            conn.handshaking = true;
                        }

                        conns_[cfd] = std::move(conn);

                        if (tls_enabled_) {
                            // 立刻推一次握手，并在握手完成后顺手读一次。
                            // 为什么不能只推握手就走：客户端常把 Finished 和第一个请求
                            // 放在同一个 TCP 段里，握手结束时那些字节已经进了 SSL 的
                            // 内部缓冲、socket 早就被读空 —— epoll 不会再为它触发
                            // EPOLLIN，请求会一直躺到 10 秒空闲回收。
                            // handle_read 内部会先判握手状态，所以这一步可以合并。
                            handle_read(cfd);
                        }
                    } else {
                        ::close(cfd);
                    }
                }
            } else if (ev & (EPOLLIN | EPOLLOUT)) {
                // 必须先处理可读/可写再处理 HUP/ERR：
                // 「发完请求立刻 close」的客户端会同时带回 EPOLLIN|EPOLLHUP，
                // 若先走 HUP 分支直接关连接，缓冲区里那条完整请求就被丢掉了。
                //
                // 这里不按事件位分流，一律走 handle_read：同一条连接在不同阶段
                // 关注的事件本来就不同——收请求是 EPOLLIN，写回是 EPOLLOUT，
                // TLS 握手可能是 EPOLLIN|EPOLLOUT。按事件位分的话，
                // 「握手需要回写」会被误送进写回逻辑，握手就永远推不完。
                // 真正的分流在 handle_read 入口按连接状态做。
                //
                // EPOLLOUT 是水平触发的：一旦注册且 socket 可写，每次 epoll_wait
                // 都会立刻返回，所以 flush_conn / drive_handshake 每次推进后都必须
                // 重新收敛关注事件，否则这里会变成 100% CPU 空转——
                // 和 EMFILE 那个自旋是同一类 bug。
                handle_read(fd);
            } else if (ev & (EPOLLHUP | EPOLLERR)) {
                close_conn(fd);
            }
        }

        // 兜底：即使 eventfd 不可用、或唤醒事件排在下一批，也不会让交还的连接一直卡着。
        drain_completed();

        // 活跃连接数由事件循环线程独占计算。读侧只能读这个原子量，
        // 绝不能去访问 conns_ —— 那张表没有锁，是事件循环线程的私有状态。
        active_conns_.store(conns_.size(), std::memory_order_relaxed);

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
        return;  // 已移交给工作线程，忽略同批残留事件
    }

    // 写回阶段不能读：此时这条连接上还有一条请求没有收尾，再解析出一条就会让它
    // 同时处理两条请求（响应交错、连接被两个任务先后关闭）。
    // 正常路径下写回阶段只注册 EPOLLOUT，压根进不到这里；这道闸是给
    // 「以后有人给写回阶段多注册一个事件位」准备的，成本是一条判断。
    if (it->second.writing) {
        flush_conn(fd);
        return;
    }

    it->second.last_active = std::chrono::steady_clock::now();

    // TLS 握手期间没有应用层字节可读，先把握手推完。
    if (it->second.handshaking && !drive_handshake(fd, it->second)) {
        return;  // 握手失败，drive_handshake 内部已经关闭连接
    }

    // drive_handshake 可能关过连接、也可能刚完成握手，迭代器要重新取。
    it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    if (it->second.handshaking) {
        return;  // 还差一轮，等下一次事件
    }

    Conn& c          = it->second;
    std::string& buf = c.read_buf;

    char tmp[8192];
    bool eof = false;
    for (;;) {
        if (c.ssl) {
            tls::Io io   = tls::Io::kOk;
            const long n = c.ssl->Read(tmp, sizeof(tmp), io);
            if (n > 0) {
                buf.append(tmp, static_cast<std::size_t>(n));
                if (buf.size() > kMaxRequestSize) {
                    close_conn(fd);  // 报文超限，无需再读
                    return;
                }
                // 继续读：SSL 内部可能还缓存着字节，而 epoll 看不到它们
                // （socket 已经被读空就不会再触发 EPOLLIN）——这是 TLS 下最典型的丢事件。
                continue;
            }
            if (io == tls::Io::kWantRead || io == tls::Io::kWantWrite) {
                // kWantWrite 在这里意味着对端在要求我们回写（TLS 1.3 不做重协商，
                // 我们又没请求客户端证书，所以基本不会发生）。不额外注册 EPOLLOUT：
                // 已经读到的字节足够继续处理，剩下的输出会在工作线程写响应时被一并冲刷。
                break;
            }
            eof = true;  // kClosed / kFailed：都不能再有数据了
            break;
        }

        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            if (buf.size() > kMaxRequestSize) {
                close_conn(fd);  // 报文超限，无需再读
                return;
            }
        } else if (n == 0) {
            eof = true;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            eof = true;
            break;
        }
    }

    HttpRequest req;
    std::size_t consumed = 0;
    int rc               = ParseRequest(buf, req, consumed);
    if (rc == kParseIncomplete && !eof) {
        return;  // 等待更多数据（空闲超时由 sweep_idle 兜底回收）
    }

    if (rc == kParseOk) {
        // 完整请求：从 epoll/conns_ 摘除，fd 所有权转移给工作线程。
        Task t;
        t.fd            = fd;
        t.req           = std::move(req);
        t.req.remote_ip = c.remote_ip;  // 必须在 conns_.erase 之前取
        // TLS 会话跟着 fd 一起走：工作线程写响应时要用它，而 SSL 对象只能有一个所有者。
        t.ssl = std::move(c.ssl);

        // 缓冲区里若还有一条完整请求（HTTP pipelining），本次交接后这些字节就丢了。
        // 本项目的客户端都是「一问一答」，不会走这条路径；但不能假装它不存在——
        // 记下这个事实，让 handle_request 放弃 keep-alive 直接关连接，
        // 客户端重连即可，比静默丢字节安全得多。
        t.pipelined = buf.size() > consumed;

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
    //
    // 但必须记账：这类请求不会进入 handler，服务端指标里如果只统计「进过 handler 的」，
    // 攻击者在指标上就是隐形的。
    requests_dropped_.fetch_add(1, std::memory_order_relaxed);
    close_conn(fd);
}

void HttpServer::handle_request(int fd, HttpRequest req, std::unique_ptr<tls::Session> ssl,
                                bool pipelined) {
    HttpResponse resp;
    resp.status = 200;

    // 异常绝不能逃出工作线程：逃出线程函数会触发 std::terminate，整个节点进程直接退出。
    // handler 会一路调到用户提供的 getter，抛异常是完全可能的正常情况。
    try {
        handler_(req, resp);
    } catch (...) {
        resp                         = HttpResponse{};
        resp.status                  = 500;
        resp.headers["Content-Type"] = "application/json";
        resp.body                    = "{\"error\":\"internal\"}";
    }

    requests_total_.fetch_add(1, std::memory_order_relaxed);
    if (resp.status >= 500) {
        responses_5xx_.fetch_add(1, std::memory_order_relaxed);
    } else if (resp.status >= 400) {
        responses_4xx_.fetch_add(1, std::memory_order_relaxed);
    } else {
        responses_2xx_.fetch_add(1, std::memory_order_relaxed);
    }

    const bool keep_alive = !pipelined && wants_keep_alive(req);

    // 不能是 const：const 的 std::string 上 std::move 只会退化成拷贝，
    // 一条 8MB 的响应会白白多复制一次（performance-move-const-arg 也会拦下来）。
    std::string data = build_response(resp, keep_alive);

    // 到这里为止一次 socket 写都没做过。整条响应交给事件循环去写，
    // 工作线程立刻回去取下一个任务——这是「慢客户端独占 worker」的修法本身。
    //
    // 以前这里是 set_nonblocking(fd, false) + SO_SNDTIMEO + 阻塞写：
    // 对端不读时 send() 会把 worker 按住整整 kSendTimeoutMs，四个这样的
    // 客户端就能让 worker_count=4 的节点对所有人失去响应。
    Completed done;
    done.fd         = fd;
    done.ssl        = std::move(ssl);
    done.remote_ip  = std::move(req.remote_ip);
    done.data       = std::move(data);
    done.keep_alive = keep_alive;
    queue_completed(std::move(done));
}

void HttpServer::queue_completed(Completed item) {
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        completed_.push_back(std::move(item));
    }

    // 立刻唤醒 epoll。不唤醒的话，这条响应要等 epoll_wait 的 100ms 超时才被写出去，
    // keep-alive 连接省下的握手时间会被这 100ms 全部吃掉——那就白做了。
    if (wake_fd_ >= 0) {
        const std::uint64_t one = 1;
        // EAGAIN 说明计数器已满，即已经处于「待唤醒」状态，无需再处理。
        const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
        (void)n;
    }
}

void HttpServer::drain_completed() {
    std::deque<Completed> pending;
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        if (completed_.empty()) {
            return;
        }
        pending.swap(completed_);
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto& r : pending) {
        // 已进入关停流程或连接数已达上限：不再挂回 epoll，直接关闭。
        // 先释放会话再关 fd，与其它关闭路径保持一致。
        if (stop_ || conns_.size() >= kMaxConns) {
            r.ssl.reset();
            ::close(r.fd);
            continue;
        }

        // 先按「等可写」注册：这条连接一出场就是写回阶段。
        // 绝大多数响应一次就写完，flush_conn 会立刻把它改回 EPOLLIN；
        // 写不完的才真的留在这里等 EPOLLOUT。
        struct epoll_event cev;
        std::memset(&cev, 0, sizeof(cev));
        cev.events  = EPOLLOUT;
        cev.data.fd = r.fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, r.fd, &cev) != 0) {
            r.ssl.reset();
            ::close(r.fd);
            continue;
        }

        Conn conn;
        conn.remote_ip        = std::move(r.remote_ip);
        conn.ssl              = std::move(r.ssl);  // 会话跟着 fd 回到事件循环
        conn.out              = std::move(r.data);
        conn.out_off          = 0;
        conn.writing          = true;
        conn.keep_alive_after = r.keep_alive;
        conn.handshaking      = false;  // 能走到这里说明握手早就完成了
        conn.last_active      = now;
        conn.write_deadline   = now + std::chrono::milliseconds(kSendTimeoutMs);
        conns_[r.fd]          = std::move(conn);

        flush_conn(r.fd);
    }
}

bool HttpServer::flush_conn(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return false;  // 同批事件里已经被关掉了
    }

    Conn& c = it->second;
    if (!c.writing) {
        return true;
    }

    std::size_t written = 0;
    while (c.out_off < c.out.size()) {
        const std::size_t left = c.out.size() - c.out_off;
        long n                 = 0;

        if (c.ssl) {
            tls::Io io = tls::Io::kOk;
            n          = c.ssl->Write(c.out.data() + c.out_off, left, io);
            if (n <= 0) {
                if (io == tls::Io::kWantWrite) {
                    set_interest(fd, EPOLLOUT);
                    return true;
                }
                // kWantRead：TLS 1.3 的 KeyUpdate 之类「写方向要先读」的情况。
                // 我们没法在读之前推进这次写，而 EPOLLOUT 是水平触发的、socket
                // 一直可写——继续重试就是 100% CPU 空转。直接关掉：
                // 客户端看到这条响应被截断，代价明确且有限。
                //
                // kClosed / kFailed 同理：半个 TLS 记录已经出去了，会话不可恢复。
                close_conn(fd);
                return false;
            }
        } else {
            const ssize_t r = ::send(fd, c.out.data() + c.out_off, left, MSG_NOSIGNAL);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    set_interest(fd, EPOLLOUT);
                    return true;
                }
                close_conn(fd);
                return false;
            }
            n = r;
        }

        c.out_off += static_cast<std::size_t>(n);
        written += static_cast<std::size_t>(n);

        // 有进展就续命：写得慢不该被掐，写不动才该。
        c.write_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kSendTimeoutMs);

        // 让出的前提是「还没写完」：刚好写满上限并且正好写完时，
        // 只多一次空转的 EPOLLOUT 才走到收尾，没必要。
        if (written >= kMaxBytesPerFlush && c.out_off < c.out.size()) {
            set_interest(fd, EPOLLOUT);  // 剩下的下一轮接着写
            return true;
        }
    }

    // 整条响应已经写出去了。
    if (!c.keep_alive_after || stop_) {
        close_conn(fd);
        return false;
    }

    // 收回成「等下一个请求」。缓冲要真的还回去：一条 keep-alive 连接留着上一次
    // 那个 8MB 的响应缓冲，是这种改法最容易长出来的内存泄漏。
    std::string().swap(c.out);
    c.out_off          = 0;
    c.writing          = false;
    c.keep_alive_after = false;
    c.last_active      = std::chrono::steady_clock::now();
    set_interest(fd, EPOLLIN);
    return true;
}

void HttpServer::worker_loop() {
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(queue_mtx_);
            // 有任务处理或收到关闭信号才唤醒（不能只等 shutdown_，否则新任务永远无人消费）。
            queue_cv_.wait(lk, [&] { return shutdown_ || !queue_.empty(); });
            if (shutdown_ && queue_.empty()) {
                return;  // 关闭信号且队列已排空
            }
            t = std::move(queue_.front());
            queue_.pop_front();
        }
        // 先取值再 move：避免依赖实参求值顺序（move 之后 t 处于未指定状态）。
        const bool pipelined = t.pipelined;
        handle_request(t.fd, std::move(t.req), std::move(t.ssl), pipelined);
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

bool HttpServer::set_interest(int fd, std::uint32_t events) {
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events  = events;
    ev.data.fd = fd;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool HttpServer::drive_handshake(int fd, Conn& c) {
    if (!c.ssl) {
        c.handshaking = false;
        return true;
    }

    // 每次推进都刷新活跃时间：握手也要受 slowloris 那套空闲回收约束，
    // 否则「连上不发 ClientHello」的连接会一直占着 fd。
    c.last_active = std::chrono::steady_clock::now();

    const tls::Step st = c.ssl->Handshake();

    if (st == tls::Step::kOk) {
        c.handshaking = false;
        // 握手期间可能为了回写注册过 EPOLLOUT，这里必须收回来。
        // 水平触发的 EPOLLOUT 一旦留着，事件循环会每轮都被唤醒并空转 100% CPU。
        set_interest(fd, EPOLLIN);
        return true;
    }

    if (st == tls::Step::kFailed) {
        // 不计入 requests_dropped：那不是「报文非法」，而是「压根没谈到 HTTP 这一层」。
        // 单独计数才有诊断价值——它涨起来基本等于「对端没开 TLS」或「证书/CA 配错了」。
        handshake_failures_.fetch_add(1, std::memory_order_relaxed);
        close_conn(fd);
        return false;
    }

    // 只关注真正需要的那一侧，避免把不需要的事件也打开造成空转。
    const std::uint32_t want = (st == tls::Step::kWantWrite)
                                   ? static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT)
                                   : static_cast<std::uint32_t>(EPOLLIN);
    set_interest(fd, want);
    return true;
}

void HttpServer::sweep_idle() {
    const auto now = std::chrono::steady_clock::now();

    // 先收集再关闭：erase 会让迭代器失效。
    std::vector<int> victims;
    for (const auto& kv : conns_) {
        const Conn& c = kv.second;

        // 写回阶段只看「写出有没有进展」，不看最后一次收数据的时刻：
        // 一条正在给慢客户端吐大响应的连接，可能已经很长时间没收过数据了，
        // 但它一直在往前推，不该被当成空闲连接掐掉。
        if (c.writing) {
            if (now >= c.write_deadline) {
                victims.push_back(kv.first);
            }
            continue;
        }

        const auto idle_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - c.last_active).count();
        if (idle_ms >= kIdleTimeoutMs) {
            victims.push_back(kv.first);
        }
    }
    for (int fd : victims) {
        close_conn(fd);
    }
}

void HttpServer::cleanup() {
    // 先把 fd 号收集起来，再清表：清表会析构每个 Conn 里的 Session（SSL_free）。
    // 顺序上「先释放会话、再关 fd」，与 handle_request 保持一致。
    std::vector<int> fds;
    fds.reserve(conns_.size());
    for (auto& kv : conns_) {
        fds.push_back(kv.first);
    }
    conns_.clear();
    for (int fd : fds) {
        ::close(fd);
    }
    active_conns_.store(0, std::memory_order_relaxed);

    // 事件循环退出后，仍可能有工作线程交还但未被 drain 的连接。
    {
        std::deque<Completed> pending;
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            pending.swap(completed_);
        }
        for (auto& r : pending) {
            r.ssl.reset();
            ::close(r.fd);
        }
    }

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

}  // namespace dcache