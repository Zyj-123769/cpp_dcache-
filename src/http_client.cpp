#include "http_client.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dcache {

namespace {

// 对端响应的大小上限。没有上限时，一个异常（或恶意）的对端只要持续发送，
// 就能把调用方节点的内存吃干净——节点间转发是内部接口，但内部接口也会出故障。
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxBodyBytes   = 8 * 1024 * 1024;

bool set_timeout(int fd, int ms, int optname) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv)) == 0;
}

int connect_timeout(const std::string& host, std::uint16_t port, int timeout_ms) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        return -1;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return -1;
    }

    // 非阻塞 connect + poll，实现可控超时。
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        ::freeaddrinfo(res);
        return -1;
    }

    struct pollfd pfd;
    pfd.fd     = fd;
    pfd.events = POLLOUT;
    int prc    = ::poll(&pfd, 1, timeout_ms);

    int so_error = 0;
    socklen_t slen = sizeof(so_error);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen);

    ::freeaddrinfo(res);

    if (prc <= 0 || so_error != 0) {
        ::close(fd);
        return -1;
    }

    ::fcntl(fd, F_SETFL, flags); // 恢复阻塞
    set_timeout(fd, timeout_ms, SO_RCVTIMEO);
    set_timeout(fd, timeout_ms, SO_SNDTIMEO);
    return fd;
}

bool send_all(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// 读入响应头（以 \r\n\r\n 结束）。
// peer_closed：还没读到任何头字节，对端就发来了 FIN。这是「复用了一条已经被
// 对端回收的连接」的典型特征，调用方据此决定是否换新连接重试。
bool read_headers(int fd, std::string& buf, std::size_t& header_end, bool& peer_closed) {
    peer_closed = false;
    char tmp[4096];
    for (;;) {
        std::size_t pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            header_end = pos + 4;
            return true;
        }
        if (buf.size() > kMaxHeaderBytes) {
            return false; // 响应头超限：对端可能根本不打算结束头部
        }
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) {
            // 连接被关闭但可能已经收到完整头。
            std::size_t pos2 = buf.find("\r\n\r\n");
            if (pos2 != std::string::npos) { header_end = pos2 + 4; return true; }
            peer_closed = buf.empty();
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

// 找到响应状态码；content_length 取 Content-Length（若存在），否则 -1。
int parse_response(const std::string& header_block, long& content_length) {
    content_length = -1;

    const std::size_t line_end = header_block.find("\r\n");
    const std::string reqline  = header_block.substr(0, line_end);

    // "HTTP/1.1 200 OK"
    const std::size_t sp1 = reqline.find(' ');
    if (sp1 == std::string::npos) return -1;
    const std::size_t sp2 = reqline.find(' ', sp1 + 1);
    const std::string status_str = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
    const int status = std::atoi(status_str.c_str());
    if (status <= 0) return -1;

    // 大小写不敏感地找 Content-Length:（本项目服务器固定发送该头）。
    auto case_find = [&](const std::string& block, const char* needle) -> std::size_t {
        std::string lower(block.size(), '\0');
        for (std::size_t i = 0; i < block.size(); ++i) {
            char c = block[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            lower[i] = c;
        }
        return lower.find(needle);
    };

    const std::size_t clp = case_find(header_block, "content-length:");
    if (clp != std::string::npos) {
        const char* p = header_block.c_str() + clp + 15; // strlen("content-length:") == 15
        content_length = std::strtol(p, nullptr, 10);
    }
    return status;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 响应头是否声明了 keep-alive。只有对端明确同意，才把连接收回池子——
// 否则会把一条「服务端马上就要关」的连接当成可复用的，下一个请求必然失败。
bool response_keeps_alive(const std::string& header_block) {
    const std::string lower = to_lower(header_block);
    return lower.find("connection: keep-alive") != std::string::npos;
}

// ---------------- 空闲连接池 ----------------

constexpr std::size_t kMaxIdlePerHost = 8;
constexpr std::size_t kMaxIdleTotal   = 64;

// 空闲连接的最长保留时间。必须明显小于服务端的空闲回收阈值（10s），
// 否则池子里会堆满「服务端早就关了、我们还以为可用」的死连接。
constexpr int kIdleTtlMs = 5000;

std::atomic<bool> g_keep_alive{true};

struct IdleConn {
    int                                   fd = -1;
    std::chrono::steady_clock::time_point since;
};

class ConnectionPool {
public:
    static ConnectionPool& Instance() {
        static ConnectionPool pool; // C++11 起函数内静态保证线程安全初始化
        return pool;
    }

    // 取一条可用连接；没有则返回 -1。
    int Take(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = idle_.find(key);
        if (it == idle_.end()) {
            return -1;
        }

        std::vector<IdleConn>& list = it->second;
        const auto now = std::chrono::steady_clock::now();
        int fd = -1;
        while (!list.empty()) {
            const IdleConn c = list.back();
            list.pop_back();
            --total_;
            if (IdleAgeMs(now, c.since) < kIdleTtlMs) {
                fd = c.fd;
                break;
            }
            ::close(c.fd); // 超时未用：服务端多半已回收，不能拿来当活连接
        }
        if (list.empty()) {
            idle_.erase(it);
        }
        return fd;
    }

    void Put(const std::string& key, int fd) {
        std::lock_guard<std::mutex> lk(mtx_);
        PurgeLocked();

        if (total_ >= kMaxIdleTotal) {
            ::close(fd);
            return;
        }
        std::vector<IdleConn>& list = idle_[key];
        if (list.size() >= kMaxIdlePerHost) {
            ::close(fd);
            return;
        }
        list.push_back(IdleConn{fd, std::chrono::steady_clock::now()});
        ++total_;
    }

    std::size_t IdleCount() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return total_;
    }

    void Clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& kv : idle_) {
            for (auto& c : kv.second) {
                ::close(c.fd);
            }
        }
        idle_.clear();
        total_ = 0;
    }

private:
    static long long IdleAgeMs(std::chrono::steady_clock::time_point now,
                               std::chrono::steady_clock::time_point since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count();
    }

    // 清掉超过 TTL 的空闲连接（调用方须持锁）。
    void PurgeLocked() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = idle_.begin(); it != idle_.end();) {
            std::vector<IdleConn>& list = it->second;
            std::size_t kept = 0;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (IdleAgeMs(now, list[i].since) < kIdleTtlMs) {
                    list[kept++] = list[i];
                } else {
                    ::close(list[i].fd);
                    --total_;
                }
            }
            list.resize(kept);
            if (list.empty()) {
                it = idle_.erase(it);
            } else {
                ++it;
            }
        }
    }

    mutable std::mutex                                     mtx_;
    std::unordered_map<std::string, std::vector<IdleConn>> idle_;
    std::size_t                                            total_ = 0;
};

enum class ExchangeResult {
    kOk,        // 完整响应，out 有效
    kStaleConn, // 一个字节都没收到、对端就关了：多半是复用的旧连接已被回收
    kFailed     // 超时、写失败、报文损坏或非 200
};

// 在一条已建立的连接上完成一次「请求 - 响应」。
// keep_alive 出参：响应完整且对端同意复用，这条连接可以收回池子。
ExchangeResult do_exchange(int fd, const std::string& host, std::uint16_t port,
                           const std::string& path, int timeout_ms,
                           const std::map<std::string, std::string>& extra_headers,
                           int* http_status, std::string& out, bool& keep_alive) {
    keep_alive = false;

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Accept: */*\r\n";
    for (const auto& h : extra_headers) {
        req += h.first + ": " + h.second + "\r\n";
    }
    req += "Connection: keep-alive\r\n\r\n";

    if (!send_all(fd, req.data(), req.size())) {
        return ExchangeResult::kFailed;
    }

    std::string buf;
    std::size_t header_end = 0;
    bool        peer_closed = false;
    if (!read_headers(fd, buf, header_end, peer_closed)) {
        return peer_closed ? ExchangeResult::kStaleConn : ExchangeResult::kFailed;
    }

    const std::string header_block = buf.substr(0, header_end);

    long content_length = -1;
    const int status    = parse_response(header_block, content_length);
    if (status <= 0) {
        return ExchangeResult::kFailed;
    }
    if (http_status != nullptr) {
        *http_status = status;
    }

    // 声明体超限就别读了：读到一半再放弃只是浪费带宽和内存。
    if (content_length > static_cast<long>(kMaxBodyBytes)) {
        return ExchangeResult::kFailed;
    }

    // 能安全复用连接的前提：响应有明确的 Content-Length（响应边界清晰），
    // 且对端明确说了 keep-alive。缺 Content-Length 时只能读到对端关闭，
    // 连接届时已经废了，谈不上复用。
    keep_alive = (content_length >= 0) && response_keeps_alive(header_block);

    std::string body;
    if (content_length >= 0) {
        const std::size_t want = static_cast<std::size_t>(content_length);

        // 先把头部之外已读到的字节计入 body。
        std::size_t body_so_far = buf.size() - header_end;
        if (body_so_far > want) {
            body_so_far = want;
        }
        body.assign(buf, header_end, body_so_far);

        char tmp[4096];
        while (body.size() < want) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            body.append(tmp, static_cast<std::size_t>(n));
            if (body.size() > kMaxBodyBytes) {
                return ExchangeResult::kFailed;
            }
        }
        if (body.size() > want) {
            body.resize(want);
        }

        // 截断必须判失败。旧实现在这里静默接受半截 body，上层拿到的是
        // 一份「看起来像 JSON」的残片，很容易被解析成错误的结论。
        if (body.size() != want) {
            return ExchangeResult::kFailed;
        }
    } else {
        // 无 Content-Length：读到对端关闭，同时受 kMaxBodyBytes 约束。
        body.assign(buf, header_end, std::string::npos);
        if (body.size() > kMaxBodyBytes) {
            return ExchangeResult::kFailed;
        }
        char tmp[4096];
        for (;;) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            body.append(tmp, static_cast<std::size_t>(n));
            if (body.size() > kMaxBodyBytes) {
                return ExchangeResult::kFailed;
            }
        }
    }

    if (status != 200) {
        return ExchangeResult::kFailed;
    }
    out = std::move(body);
    return ExchangeResult::kOk;
}

} // namespace

bool HttpClient::Get(const std::string& host, std::uint16_t port,
                     const std::string& path, std::string& out, int timeout_ms,
                     const std::map<std::string, std::string>& extra_headers,
                     int* http_status) {
    if (http_status != nullptr) {
        *http_status = 0; // 默认：连状态行都没读到
    }

    const std::string key         = host + ":" + std::to_string(port);
    const bool        allow_reuse = g_keep_alive.load(std::memory_order_relaxed);

    // 最多两轮：第二轮只会发生在「复用的旧连接已经失效」时。
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool reused = false;
        int  fd     = -1;

        if (allow_reuse) {
            fd     = ConnectionPool::Instance().Take(key);
            reused = fd >= 0;
        }
        if (fd < 0) {
            fd     = connect_timeout(host, port, timeout_ms);
            reused = false;
        }
        if (fd < 0) {
            return false;
        }

        int         status     = 0;
        std::string body;
        bool        keep_alive = false;
        const ExchangeResult r = do_exchange(fd, host, port, path, timeout_ms, extra_headers,
                                             &status, body, keep_alive);

        if (http_status != nullptr && status != 0) {
            *http_status = status;
        }

        if (r == ExchangeResult::kOk) {
            if (keep_alive && allow_reuse) {
                ConnectionPool::Instance().Put(key, fd);
            } else {
                ::close(fd);
            }
            out = std::move(body);
            return true;
        }

        ::close(fd);

        // 只在「复用连接上一个字节都没收到」时重试：这说明这条连接在池子里放久了、
        // 已被对端回收，换条新连接重发即可（GET 幂等）。
        // 超时与写错误一律不重试——对端可能正在处理，重发等于把故障放大一倍。
        if (r == ExchangeResult::kStaleConn && reused) {
            continue;
        }
        return false;
    }
    return false;
}

void HttpClient::SetKeepAlive(bool on) {
    g_keep_alive.store(on, std::memory_order_relaxed);
}

std::size_t HttpClient::IdleConnections() {
    return ConnectionPool::Instance().IdleCount();
}

void HttpClient::CloseIdleConnections() {
    ConnectionPool::Instance().Clear();
}

} // namespace dcache