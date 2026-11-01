#include "tls.hpp"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>

namespace dcache {
namespace tls {

namespace {

// 最低 TLS 1.2。1.0/1.1 已被各大浏览器和主流发行版弃用，留着的唯一效果
// 是给降级攻击多一个入口。
constexpr int kMinVersion = TLS1_2_VERSION;

// 阻塞 fd 上 SSL_write 返回 WANT_* 的重试上限。
// 正常情况下 cipher 状态机一两次就推进完了；给上限只是为了「绝不无限循环」——
// 死循环在事件循环里是灾难，在工作线程里同样会把一个 worker 永久占用。
constexpr int kMaxWriteSpins = 16;

std::string drain_errors() {
    std::string out;
    char buf[256];
    unsigned long e = 0;
    while ((e = ::ERR_get_error()) != 0) {
        ::ERR_error_string_n(e, buf, sizeof(buf));
        if (!out.empty()) {
            out += "; ";
        }
        out += buf;
    }
    ::ERR_clear_error();
    return out.empty() ? std::string("unknown TLS error") : out;
}

// 连接目标是不是 IPv4 字面量。决定 SNI 与证书校验该走哪条路（见 Connect）。
bool is_ipv4_literal(const std::string& host) {
    struct in_addr a;
    return ::inet_pton(AF_INET, host.c_str(), &a) == 1;
}

// SSL_read/SSL_write 的长度参数是 int，调用方给的是 size_t。
// 本项目的缓冲区都在 8KB 以内，这里只做防御性截断，不改变正常语义。
int clamp_len(std::size_t len) {
    return len > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(len);
}

// OpenSSL 的默认 BIO 用 write(2)，不像本项目自己的 send_all / write_all 那样
// 带 MSG_NOSIGNAL。于是「客户端发完请求立刻断开，服务端正在写响应」这种再正常
// 不过的行为会让进程收到 SIGPIPE 直接退出 —— **一个客户端就能杀掉节点**。
//
// 这个缺陷躲过了 ASan/UBSan、TSan、fuzz 和四批人工 review，因为之前六组 TLS
// 用例里客户端都是「写完再读、读够才关」，从没构造出「写到已关闭连接」的那一刻。
// 是证书热加载那组用例把连接寿命拉长之后才第一次踩到。
//
// 修法只可能是忽略 SIGPIPE：忽略之后 SSL_write 返回 -1（EPIPE），走的是已有的
// 写失败分支——关掉这条连接，进程继续服务。不给 SSL 换自定义 BIO（用带
// MSG_NOSIGNAL 的 send）是因为那要维护读写回调、处理 SSL_set_bio 的所有权转移，
// 改动面大一个数量级，而结果完全一样。
void ensure_sigpipe_ignored() {
    static std::once_flag once;
    std::call_once(once, []() {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        ::sigaction(SIGPIPE, &sa, nullptr);
    });
}

}  // namespace

bool Available() {
    return true;
}

std::string Backend() {
    return std::string("openssl ") + ::OpenSSL_version(OPENSSL_VERSION);
}

// ---------------- Context ----------------

Context::~Context() {
    if (ctx_ != nullptr) {
        ::SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
    }
}

std::unique_ptr<Context> Context::CreateServer(const ServerConfig& cfg, std::string& err) {
    // 只要这个进程有 TLS 服务端，就一定会走 OpenSSL 的写路径，必须在建任何
    // 上下文之前先把 SIGPIPE 关掉（理由见 ensure_sigpipe_ignored）。
    ensure_sigpipe_ignored();

    if (cfg.cert_file.empty() || cfg.key_file.empty()) {
        err = "TLS 服务端配置缺少 cert_file 或 key_file";
        return nullptr;
    }

    ::ERR_clear_error();
    SSL_CTX* ctx = ::SSL_CTX_new(::TLS_server_method());
    if (ctx == nullptr) {
        err = "SSL_CTX_new: " + drain_errors();
        return nullptr;
    }

    auto fail = [&](const std::string& what) -> std::unique_ptr<Context> {
        err = what + ": " + drain_errors();
        ::SSL_CTX_free(ctx);
        return nullptr;
    };

    if (::SSL_CTX_set_min_proto_version(ctx, kMinVersion) != 1) {
        return fail("设置最低 TLS 版本");
    }
    ::SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);

    // 空闲 keep-alive 连接不占着每连接 ~16KB×2 的读写缓冲。
    // 这个节点默认允许 8192 条挂起连接，不释放的话光缓冲就能吃掉 256MB。
    ::SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);

    if (::SSL_CTX_use_certificate_chain_file(ctx, cfg.cert_file.c_str()) != 1) {
        return fail("加载证书 " + cfg.cert_file);
    }
    if (::SSL_CTX_use_PrivateKey_file(ctx, cfg.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        return fail("加载私钥 " + cfg.key_file);
    }
    // 证书与私钥不匹配时 handshake 会在运行时对每个连接失败，
    // 在配置阶段就拦住，比让运维去看 handshake_failures 计数好得多。
    if (::SSL_CTX_check_private_key(ctx) != 1) {
        return fail("证书与私钥不匹配");
    }

    // 不请求客户端证书。理由：集群里每个节点跑的是同一个二进制，身份靠
    // X-DCache-Token 做应用层授权；上 mTLS 要为每台机器签发并维护吊销列表，
    // 换来的只是「同 token 的不同节点无法互相冒用」——在共享 token 模型下没有净收益。
    ::SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    return std::unique_ptr<Context>(new Context(ctx));
}

std::unique_ptr<Context> Context::CreateClient(const ClientConfig& cfg, std::string& err) {
    // 客户端同样要关：转发的对端也可能在我们写请求时先断开。
    ensure_sigpipe_ignored();

    if (cfg.ca_file.empty()) {
        err = "TLS 客户端缺少 CA 文件（不校验证书的 TLS 只是把明文换了个包装）";
        return nullptr;
    }

    ::ERR_clear_error();
    SSL_CTX* ctx = ::SSL_CTX_new(::TLS_client_method());
    if (ctx == nullptr) {
        err = "SSL_CTX_new: " + drain_errors();
        return nullptr;
    }

    auto fail = [&](const std::string& what) -> std::unique_ptr<Context> {
        err = what + ": " + drain_errors();
        ::SSL_CTX_free(ctx);
        return nullptr;
    };

    if (::SSL_CTX_set_min_proto_version(ctx, kMinVersion) != 1) {
        return fail("设置最低 TLS 版本");
    }
    ::SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    ::SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);

    // 校验对端证书。这是整个 TLS 里最容易被省掉、也最不能省的一步：
    // 关掉它之后，任何能改路由的攻击者都能用一张自签证书把中间人做得天衣无缝。
    ::SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (::SSL_CTX_load_verify_locations(ctx, cfg.ca_file.c_str(), nullptr) != 1) {
        return fail("加载 CA " + cfg.ca_file);
    }

    return std::unique_ptr<Context>(new Context(ctx));
}

// ---------------- Session ----------------

Session::~Session() {
    if (ssl_ != nullptr) {
        // 刻意不调 SSL_shutdown：它要发 close_notify 并等对端回应，最坏再耗掉一个
        // SO_SNDTIMEO 周期，而工作线程这时候本该去服务下一个请求。
        // 代价是放弃「可检测的截断」（对端能分辨正常关闭与连接被砍），
        // 但我们只在响应写完整或本来就要丢弃连接时才走到这里，不值得为此多一个
        // 阻塞点。这是明确的取舍，不是忘了调。
        ::SSL_free(static_cast<SSL*>(ssl_));
    }
}

std::unique_ptr<Session> Session::Accept(Context& ctx, int fd) {
    ::ERR_clear_error();
    SSL* ssl = ::SSL_new(static_cast<SSL_CTX*>(ctx.native()));
    if (ssl == nullptr) {
        return nullptr;
    }
    if (::SSL_set_fd(ssl, fd) != 1) {
        ::SSL_free(ssl);
        return nullptr;
    }
    return std::unique_ptr<Session>(new Session(ssl, true));
}

std::unique_ptr<Session> Session::Connect(Context& ctx, int fd, const std::string& host) {
    ::ERR_clear_error();
    SSL* ssl = ::SSL_new(static_cast<SSL_CTX*>(ctx.native()));
    if (ssl == nullptr) {
        return nullptr;
    }

    // SNI 与证书校验必须按「IP 字面量 / 域名」分开处理：
    //   - RFC 6066 不允许把 IP 放进 SNI，有些对端见到会直接断连；
    //   - SSL_set1_host 只按 DNS 名校验，"127.0.0.1" 交给它会永远失败，
    //     正确入口是 X509_VERIFY_PARAM_set1_ip_asc（匹配 iPAddress SAN）。
    // 这是 curl 历史上踩过的经典坑：漏掉分支后「用 IP 连集群」直接连不上，
    // 而报错只说「证书不匹配」，看不出是校验方式选错了。
    if (is_ipv4_literal(host)) {
        if (::X509_VERIFY_PARAM_set1_ip_asc(::SSL_get0_param(ssl), host.c_str()) != 1) {
            ::SSL_free(ssl);
            return nullptr;
        }
    } else {
        if (::SSL_set_tlsext_host_name(ssl, host.c_str()) != 1) {
            ::SSL_free(ssl);
            return nullptr;
        }
        if (::SSL_set1_host(ssl, host.c_str()) != 1) {
            ::SSL_free(ssl);
            return nullptr;
        }
    }

    if (::SSL_set_fd(ssl, fd) != 1) {
        ::SSL_free(ssl);
        return nullptr;
    }
    return std::unique_ptr<Session>(new Session(ssl, false));
}

Step Session::Handshake() {
    ::ERR_clear_error();
    const int r = server_side_ ? ::SSL_accept(static_cast<SSL*>(ssl_))
                               : ::SSL_connect(static_cast<SSL*>(ssl_));
    if (r == 1) {
        return Step::kOk;
    }
    switch (::SSL_get_error(static_cast<SSL*>(ssl_), r)) {
    case SSL_ERROR_WANT_READ:
        return Step::kWantRead;
    case SSL_ERROR_WANT_WRITE:
        return Step::kWantWrite;
    default:
        return Step::kFailed;
    }
}

long Session::Read(void* buf, std::size_t len, Io& io) {
    if (len == 0) {
        io = Io::kOk;
        return 0;
    }

    ::ERR_clear_error();
    SSL* ssl    = static_cast<SSL*>(ssl_);
    const int n = ::SSL_read(ssl, buf, clamp_len(len));
    if (n > 0) {
        io = Io::kOk;
        return n;
    }

    switch (::SSL_get_error(ssl, n)) {
    case SSL_ERROR_WANT_READ:
        io = Io::kWantRead;
        return -1;
    case SSL_ERROR_WANT_WRITE:
        io = Io::kWantWrite;
        return -1;
    case SSL_ERROR_ZERO_RETURN:
        // 对端发了 close_notify：这是 TLS 定义的正常关闭。
        io = Io::kClosed;
        return 0;
    case SSL_ERROR_SYSCALL:
        // 阻塞 fd 上 SO_RCVTIMEO 到期、或非阻塞 fd 上的 EAGAIN 都落到这里。
        // 必须在失败分支之前判掉：否则一次正常的「暂时没数据」会被当成
        // 连接损坏，keep-alive 会在空闲几秒后被自己掐断。
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            io = Io::kWantRead;
            return -1;
        }
        if (n == 0) {
            // 还没收到 close_notify 对端就没了。按关闭处理：
            // 上层只会保留已读到的完整请求，半截请求本来就要丢弃。
            io = Io::kClosed;
            return 0;
        }
        io = Io::kFailed;
        return -1;
    default:
        io = Io::kFailed;
        return -1;
    }
}

bool Session::WriteAll(const void* data, std::size_t len) {
    SSL* ssl        = static_cast<SSL*>(ssl_);
    const char* p   = static_cast<const char*>(data);
    std::size_t off = 0;
    int spins       = 0;

    while (off < len) {
        ::ERR_clear_error();
        const int n = ::SSL_write(ssl, p + off, clamp_len(len - off));
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            spins = 0;
            continue;
        }

        const int e = ::SSL_get_error(ssl, n);
        if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) && ++spins <= kMaxWriteSpins) {
            // 原样重试：SSL_write 要求 WANT_* 后用**同一个**缓冲区重试，
            // 这里指针和长度都没变（offset 也没推进），满足这个前提。
            continue;
        }
        return false;
    }
    return true;
}

std::string Session::LastError() const {
    return drain_errors();
}

}  // namespace tls
}  // namespace dcache
