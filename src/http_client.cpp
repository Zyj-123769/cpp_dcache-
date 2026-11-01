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
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
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

// TLS 的全局上下文。
//
// 为什么用 shared_ptr + atomic_load/store 而不是一把锁：Get 在转发热路径上，
// 每请求都要判断「这次走不走 TLS」，加锁等于给所有转发串行化。
// 这和 peer_pool 换哈希环用的是同一套写法——读侧拿快照，写侧换指针，
// 已经取到旧快照的线程继续安全用到它用完为止。
std::shared_ptr<tls::Context> g_tls_ctx;

std::shared_ptr<tls::Context> TlsContext() {
    return std::atomic_load(&g_tls_ctx);
}

// 阻塞 fd 上重试 TLS WANT_* 的上限。阻塞 fd 上每重试一次都要等一个
// SO_RCVTIMEO 周期，所以循环本身已经是有界的；上限只是保证「绝不无限循环」。
constexpr int kMaxIoSpins = 8;

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

    // 逐个尝试全部解析结果，而不是只用第一个。
    // 只用第一个的后果很具体：多数 Linux 上 `localhost` 先解析到 ::1，
    // 而节点监听的是 IPv4，于是「按域名连本机」直接失败，报错还只是「连不上」，
    // 看不出是地址族选错了。所有候选共享同一个超时预算，避免 N 个地址把
    // 一次转发的耗时翻成 N 倍。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            break;
        }

        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        // 非阻塞 connect + poll，实现可控超时。
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            ::close(fd);
            continue;  // 这个地址不通，试下一个
        }

        if (rc < 0) {
            struct pollfd pfd;
            pfd.fd      = fd;
            pfd.events  = POLLOUT;
            pfd.revents = 0;
            if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
                ::close(fd);
                continue;
            }
        }

        int so_error   = 0;
        socklen_t slen = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen);
        if (so_error != 0) {
            ::close(fd);
            continue;
        }

        ::fcntl(fd, F_SETFL, flags);  // 恢复阻塞
        set_timeout(fd, timeout_ms, SO_RCVTIMEO);
        set_timeout(fd, timeout_ms, SO_SNDTIMEO);
        ::freeaddrinfo(res);
        return fd;
    }

    ::freeaddrinfo(res);
    return -1;
}

bool send_all(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// ---------------- TLS 辅助 ----------------

// 一次读的结果。kError 与 kEof 在上层都要收尾，但语义不同：
// 前者是「连接坏了」（超时/握手后的协议错误），后者是「对端正常收尾」。
enum class ReadOutcome { kData, kEof, kError };

// 统一的一次读：有 TLS 会话就走 SSL，否则走裸 recv。
ReadOutcome conn_read(int fd, tls::Session* ssl, char* buf, std::size_t len, std::size_t& got) {
    got = 0;

    if (ssl == nullptr) {
        for (;;) {
            const ssize_t n = ::recv(fd, buf, len, 0);
            if (n > 0) {
                got = static_cast<std::size_t>(n);
                return ReadOutcome::kData;
            }
            if (n == 0) {
                return ReadOutcome::kEof;
            }
            if (errno == EINTR) {
                continue;
            }
            return ReadOutcome::kError;
        }
    }

    for (int spins = 0; spins < kMaxIoSpins; ++spins) {
        tls::Io io   = tls::Io::kOk;
        const long n = ssl->Read(buf, len, io);
        if (n > 0) {
            got = static_cast<std::size_t>(n);
            return ReadOutcome::kData;
        }
        if (io == tls::Io::kClosed) {
            return ReadOutcome::kEof;
        }
        if (io == tls::Io::kWantRead || io == tls::Io::kWantWrite) {
            // fd 是阻塞的：这里再调一次 SSL_read 会阻塞到数据到达或超时，
            // 所以「重试」不会变成忙等。
            continue;
        }
        return ReadOutcome::kError;
    }
    return ReadOutcome::kError;
}

bool conn_send(int fd, tls::Session* ssl, const char* data, std::size_t len) {
    if (ssl != nullptr) {
        return ssl->WriteAll(data, len);
    }
    return send_all(fd, data, len);
}

// 在给定预算内完成客户端侧握手。
//
// 为什么不直接靠阻塞 fd 上的 SO_RCVTIMEO：一次握手要来回好几趟，每趟都可能等满
// 一个超时周期，最坏情况是「超时 × 往返数」，一个不响应的对端能把一次转发拖成十几秒。
// 这里临时切非阻塞 + poll 并显式算 deadline，让握手耗时等于调用方给的 timeout。
bool do_handshake(int fd, tls::Session& ssl, int timeout_ms) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }

    bool ok             = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    for (;;) {
        const tls::Step st = ssl.Handshake();
        if (st == tls::Step::kOk) {
            ok = true;
            break;
        }
        if (st == tls::Step::kFailed) {
            break;
        }

        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            break;
        }

        struct pollfd pfd;
        pfd.fd      = fd;
        pfd.events  = (st == tls::Step::kWantWrite) ? POLLOUT : POLLIN;
        pfd.revents = 0;
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            break;  // 超时或被信号打断：都不再重试
        }
    }

    // 恢复阻塞模式：后面的 SSL_read / SSL_write 依赖 SO_RCVTIMEO 来限时。
    ::fcntl(fd, F_SETFL, flags);
    return ok;
}

// 读入响应头（以 \r\n\r\n 结束）。
// peer_closed：还没读到任何头字节，对端就发来了 FIN。这是「复用了一条已经被
// 对端回收的连接」的典型特征，调用方据此决定是否换新连接重试。
bool read_headers(int fd, tls::Session* ssl, std::string& buf, std::size_t& header_end,
                  bool& peer_closed) {
    peer_closed = false;
    char tmp[4096];
    for (;;) {
        std::size_t pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            header_end = pos + 4;
            return true;
        }
        if (buf.size() > kMaxHeaderBytes) {
            return false;  // 响应头超限：对端可能根本不打算结束头部
        }

        std::size_t got     = 0;
        const ReadOutcome r = conn_read(fd, ssl, tmp, sizeof(tmp), got);
        if (r == ReadOutcome::kEof) {
            // 连接被关闭但可能已经收到完整头。
            std::size_t pos2 = buf.find("\r\n\r\n");
            if (pos2 != std::string::npos) {
                header_end = pos2 + 4;
                return true;
            }
            peer_closed = buf.empty();
            return false;
        }
        if (r == ReadOutcome::kError) {
            return false;
        }
        buf.append(tmp, got);
    }
}

// 找到响应状态码；content_length 取 Content-Length（若存在），否则 -1。
int parse_response(const std::string& header_block, long& content_length) {
    content_length = -1;

    const std::size_t line_end = header_block.find("\r\n");
    const std::string reqline  = header_block.substr(0, line_end);

    // "HTTP/1.1 200 OK"
    const std::size_t sp1 = reqline.find(' ');
    if (sp1 == std::string::npos)
        return -1;
    const std::size_t sp2        = reqline.find(' ', sp1 + 1);
    const std::string status_str = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
    const int status             = std::atoi(status_str.c_str());
    if (status <= 0)
        return -1;

    // 大小写不敏感地找 Content-Length:（本项目服务器固定发送该头）。
    auto case_find = [&](const std::string& block, const char* needle) -> std::size_t {
        std::string lower(block.size(), '\0');
        for (std::size_t i = 0; i < block.size(); ++i) {
            char c = block[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            lower[i] = c;
        }
        return lower.find(needle);
    };

    const std::size_t clp = case_find(header_block, "content-length:");
    if (clp != std::string::npos) {
        const char* p  = header_block.c_str() + clp + 15;  // strlen("content-length:") == 15
        content_length = std::strtol(p, nullptr, 10);
    }
    return status;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
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
    int fd = -1;
    // TLS 会话（明文模式下为 nullptr）。会话必须跟连接一起进出池子：
    // 把 SSL 丢掉、只留 fd，等于让下一条请求在一条已握手的连接上发明文。
    std::unique_ptr<tls::Session> ssl;
    std::chrono::steady_clock::time_point since;
};

class ConnectionPool {
public:
    static ConnectionPool& Instance() {
        static ConnectionPool pool;  // C++11 起函数内静态保证线程安全初始化
        return pool;
    }

    // 取一条可用连接；没有则返回 fd == -1。
    IdleConn Take(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = idle_.find(key);
        if (it == idle_.end()) {
            return IdleConn{};
        }

        std::vector<IdleConn>& list = it->second;
        const auto now              = std::chrono::steady_clock::now();
        while (!list.empty()) {
            IdleConn c = std::move(list.back());
            list.pop_back();
            --total_;
            if (IdleAgeMs(now, c.since) < kIdleTtlMs) {
                if (list.empty()) {
                    idle_.erase(it);
                }
                return c;
            }
            ::close(c.fd);  // 超时未用：服务端多半已回收，不能拿来当活连接
        }
        idle_.erase(it);
        return IdleConn{};
    }

    void Put(const std::string& key, IdleConn conn) {
        std::lock_guard<std::mutex> lk(mtx_);
        PurgeLocked();

        if (total_ >= kMaxIdleTotal) {
            ::close(conn.fd);
            return;
        }
        std::vector<IdleConn>& list = idle_[key];
        if (list.size() >= kMaxIdlePerHost) {
            ::close(conn.fd);
            return;
        }
        conn.since = std::chrono::steady_clock::now();
        list.push_back(std::move(conn));
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
    //
    // 用 erase 而不是「原地往前挪」的经典写法：IdleConn 带着 unique_ptr，
    // 挪动必须用移动赋值/swap，写成 list[kept++] = list[i] 会退化成拷贝赋值
    // ——而拷贝已经被 unique_ptr 删掉了，编译器报的是「use of deleted function」，
    // 很容易被误读成「IdleConn 不可拷贝」这种设计问题。
    // 每个 host 最多 kMaxIdlePerHost(8) 条，O(n²) 在这里没有意义。
    void PurgeLocked() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = idle_.begin(); it != idle_.end();) {
            std::vector<IdleConn>& list = it->second;
            std::size_t i               = 0;
            while (i < list.size()) {
                if (IdleAgeMs(now, list[i].since) < kIdleTtlMs) {
                    ++i;
                    continue;
                }
                ::close(list[i].fd);  // 会话由被删元素的析构释放
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                --total_;
            }
            if (list.empty()) {
                it = idle_.erase(it);
            } else {
                ++it;
            }
        }
    }

    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::vector<IdleConn>> idle_;
    std::size_t total_ = 0;
};

enum class ExchangeResult {
    kOk,         // 完整响应，out 有效
    kStaleConn,  // 一个字节都没收到、对端就关了：多半是复用的旧连接已被回收
    kFailed      // 超时、写失败、报文损坏或非 200
};

// 在一条已建立的连接上完成一次「请求 - 响应」。
// keep_alive 出参：响应完整且对端同意复用，这条连接可以收回池子。
ExchangeResult do_exchange(int fd, tls::Session* ssl, const std::string& host, std::uint16_t port,
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

    if (!conn_send(fd, ssl, req.data(), req.size())) {
        return ExchangeResult::kFailed;
    }

    std::string buf;
    std::size_t header_end = 0;
    bool peer_closed       = false;
    if (!read_headers(fd, ssl, buf, header_end, peer_closed)) {
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
            std::size_t got     = 0;
            const ReadOutcome r = conn_read(fd, ssl, tmp, sizeof(tmp), got);
            if (r == ReadOutcome::kEof) {
                break;
            }
            if (r == ReadOutcome::kError) {
                break;
            }
            body.append(tmp, got);
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
            std::size_t got     = 0;
            const ReadOutcome r = conn_read(fd, ssl, tmp, sizeof(tmp), got);
            if (r != ReadOutcome::kData) {
                break;  // 对端关了就结束；读错误同样只能到此为止
            }
            body.append(tmp, got);
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

}  // namespace

bool HttpClient::Get(const std::string& host, std::uint16_t port, const std::string& path,
                     std::string& out, int timeout_ms,
                     const std::map<std::string, std::string>& extra_headers, int* http_status) {
    if (http_status != nullptr) {
        *http_status = 0;  // 默认：连状态行都没读到
    }

    const std::shared_ptr<tls::Context> tls_ctx = TlsContext();

    // 池键必须带协议前缀。理由不是洁癖：一旦运行时切换过 TLS（测试里必然会），
    // 池里就可能躺着另一种协议的连接；按 host:port 取值会把一条明文连接交给
    // TLS 请求，症状是「第一个请求正常、之后随机失败」，排查成本极高。
    const std::string key =
        std::string(tls_ctx ? "tls://" : "tcp://") + host + ":" + std::to_string(port);
    const bool allow_reuse = g_keep_alive.load(std::memory_order_relaxed);

    // 最多两轮：第二轮只会发生在「复用的旧连接已经失效」时。
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool reused = false;
        IdleConn conn;

        if (allow_reuse) {
            conn   = ConnectionPool::Instance().Take(key);
            reused = conn.fd >= 0;
        }
        if (conn.fd < 0) {
            conn.fd = connect_timeout(host, port, timeout_ms);
            reused  = false;
            if (conn.fd < 0) {
                return false;
            }
            if (tls_ctx) {
                conn.ssl = tls::Session::Connect(*tls_ctx, conn.fd, host);
                if (!conn.ssl || !do_handshake(conn.fd, *conn.ssl, timeout_ms)) {
                    // 握手失败不重试：几乎总是「对端没开 TLS」或「证书/CA 不对」，
                    // 重试只会把一次 2 秒的超时变成两次，故障时反而更慢。
                    conn.ssl.reset();
                    ::close(conn.fd);
                    return false;
                }
            }
        }

        int status = 0;
        std::string body;
        bool keep_alive        = false;
        const ExchangeResult r = do_exchange(conn.fd, conn.ssl.get(), host, port, path, timeout_ms,
                                             extra_headers, &status, body, keep_alive);

        if (http_status != nullptr && status != 0) {
            *http_status = status;
        }

        if (r == ExchangeResult::kOk) {
            if (keep_alive && allow_reuse) {
                ConnectionPool::Instance().Put(key, std::move(conn));
            } else {
                conn.ssl.reset();
                ::close(conn.fd);
            }
            out = std::move(body);
            return true;
        }

        conn.ssl.reset();
        ::close(conn.fd);

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

bool HttpClient::EnableTls(const tls::ClientConfig& cfg, std::string& err) {
    std::shared_ptr<tls::Context> ctx = tls::Context::CreateClient(cfg, err);
    if (!ctx) {
        return false;
    }

    // 先清池、再换指针。反过来的话，新请求会拿着 TLS 的池键去取旧连接——
    // 虽然池键带了协议前缀、取不到，但那些明文连接会一直躺到 TTL 才释放，
    // 白白占着 fd 和对端资源。
    ConnectionPool::Instance().Clear();
    std::atomic_store(&g_tls_ctx, std::move(ctx));
    return true;
}

bool HttpClient::TlsEnabled() {
    return static_cast<bool>(TlsContext());
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

}  // namespace dcache