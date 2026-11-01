#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "consistent_hash.hpp"
#include "group.hpp"
#include "http.hpp"
#include "http_client.hpp"
#include "http_server.hpp"
#include "node.hpp"
#include "tls.hpp"

using namespace dcache;

namespace {

// 测试用的自签证书。
//
// 为什么在运行时签发，而不是签一份 PEM 塞进仓库：
//   - 签入的证书必然过期，这份测试会在某个未来时间点自己变红；
//   - 更糟的是它会让人误以为「测试通过」等于「生产证书配对了」。
// 现场签发要多写几十行，但它证明的是当下这台机器的 OpenSSL 真的能握手。
struct CertFiles {
    std::string cert_path;
    std::string key_path;
};

// 建一个临时文件并返回路径。用 mkstemp 而不是自己拼 pid + 时间戳：
// 并发的测试进程（ctest -j）撞名字的概率不为零，而撞上之后的报错会指向
// 「证书加载失败」，完全看不出是路径冲突。
std::string temp_path() {
    std::string tmpl = "/tmp/dcache_tls_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    const int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        return std::string();
    }
    ::close(fd);
    return std::string(buf.data());
}

bool write_file(const std::string& path, const std::string& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const std::size_t n = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return n == data.size();
}

std::string bio_to_string(BIO* bio) {
    char* p      = nullptr;
    const long n = BIO_get_mem_data(bio, &p);
    if (n <= 0 || p == nullptr) {
        return std::string();
    }
    return std::string(p, static_cast<std::size_t>(n));
}

// 生成一对自签的 cert/key。san_ip=true 时 SAN 里带 IP:127.0.0.1。
// 不带 IP SAN 的那份用来验证「按 IP 连接必须失败」——这是 SSL_set1_host 与
// set1_ip_asc 走错分支时最典型的坑。
bool make_self_signed(const char* cn, bool san_ip, CertFiles& out) {
    bool ok             = false;
    EVP_PKEY_CTX* pctx  = nullptr;
    EVP_PKEY* pkey      = nullptr;
    X509* x             = nullptr;
    X509_EXTENSION* ext = nullptr;
    BIO* cert_bio       = nullptr;
    BIO* key_bio        = nullptr;

    do {
        pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (pctx == nullptr) {
            break;
        }
        if (EVP_PKEY_keygen_init(pctx) != 1) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) != 1) {
            break;
        }
        if (EVP_PKEY_keygen(pctx, &pkey) != 1) {
            break;
        }

        x = X509_new();
        if (x == nullptr) {
            break;
        }
        if (X509_set_version(x, 2) != 1) {  // v3，SAN 需要
            break;
        }
        if (ASN1_INTEGER_set(X509_get_serialNumber(x), 1) != 1) {
            break;
        }
        if (X509_gmtime_adj(X509_getm_notBefore(x), -60) == nullptr) {
            break;
        }
        if (X509_gmtime_adj(X509_getm_notAfter(x), 86400) == nullptr) {
            break;
        }
        if (X509_set_pubkey(x, pkey) != 1) {
            break;
        }

        X509_NAME* name = X509_get_subject_name(x);
        if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(cn), -1, -1,
                                       0) != 1) {
            break;
        }
        if (X509_set_issuer_name(x, name) != 1) {  // 自签：issuer == subject
            break;
        }

        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
        const std::string san = san_ip ? "DNS:localhost,IP:127.0.0.1" : "DNS:localhost";
        ext = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, san.c_str());
        if (ext == nullptr) {
            break;
        }
        if (X509_add_ext(x, ext, -1) != 1) {
            break;
        }

        if (X509_sign(x, pkey, EVP_sha256()) == 0) {
            break;
        }

        cert_bio = BIO_new(BIO_s_mem());
        key_bio  = BIO_new(BIO_s_mem());
        if (cert_bio == nullptr || key_bio == nullptr) {
            break;
        }
        if (PEM_write_bio_X509(cert_bio, x) != 1) {
            break;
        }
        if (PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            break;
        }

        out.cert_path = temp_path();
        out.key_path  = temp_path();
        if (out.cert_path.empty() || out.key_path.empty()) {
            break;
        }
        if (!write_file(out.cert_path, bio_to_string(cert_bio))) {
            break;
        }
        if (!write_file(out.key_path, bio_to_string(key_bio))) {
            break;
        }
        ok = true;
    } while (false);

    // 全部释放：测试跑在 ASan 下，LeakSanitizer 会把漏掉的 X509/EVP_PKEY 判成失败。
    if (cert_bio != nullptr) {
        BIO_free(cert_bio);
    }
    if (key_bio != nullptr) {
        BIO_free(key_bio);
    }
    if (ext != nullptr) {
        X509_EXTENSION_free(ext);
    }
    if (x != nullptr) {
        X509_free(x);
    }
    if (pkey != nullptr) {
        EVP_PKEY_free(pkey);
    }
    if (pctx != nullptr) {
        EVP_PKEY_CTX_free(pctx);
    }
    return ok;
}

void remove_files(CertFiles& c) {
    if (!c.cert_path.empty()) {
        ::unlink(c.cert_path.c_str());
        c.cert_path.clear();
    }
    if (!c.key_path.empty()) {
        ::unlink(c.key_path.c_str());
        c.key_path.clear();
    }
}

// 裸 socket 发一段字节，读完对端给的一切（或超时）。
// 用它构造 HttpClient 做不到的场景：往 TLS 端口灌明文。
std::string raw_exchange(std::uint16_t port, const std::string& request) {
    std::string out;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return out;
    }

    struct timeval tv;
    tv.tv_sec  = 2;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) != 0) {
        ::close(fd);
        return out;
    }

    std::size_t off = 0;
    while (off < request.size()) {
        const ssize_t n = ::send(fd, request.data() + off, request.size() - off, 0);
        if (n <= 0) {
            break;
        }
        off += static_cast<std::size_t>(n);
    }

    char tmp[1024];
    for (;;) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        out.append(tmp, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

std::string read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return std::string();
    }
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

// 最小 TLS 客户端，**只用于测量**服务端此刻发的是哪张证书。
//
// 刻意 SSL_VERIFY_NONE：这里要证明的是「轮换生效了」，一旦开启校验，
// 「换成新证书后旧 CA 连不上」会先让连接断开，反而看不到新证书是谁。
// 这是测量工具，不是产品路径 —— 产品路径在 HttpClient 里，那里强制校验，
// 且有一条专门的用例证明它在错误的 CA 下必须失败。
class TlsProbe {
public:
    TlsProbe() = default;
    ~TlsProbe() { close(); }
    TlsProbe(const TlsProbe&)            = delete;
    TlsProbe& operator=(const TlsProbe&) = delete;

    bool open(std::uint16_t port) {
        close();
        ctx_ = ::SSL_CTX_new(::TLS_client_method());
        if (ctx_ == nullptr) {
            return false;
        }
        ::SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);

        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return false;
        }
        struct timeval tv;
        tv.tv_sec  = 3;
        tv.tv_usec = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in sin;
        std::memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port   = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) != 1 ||
            ::connect(fd_, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) != 0) {
            return false;
        }

        ssl_ = ::SSL_new(ctx_);
        if (ssl_ == nullptr) {
            return false;
        }
        if (::SSL_set_fd(ssl_, fd_) != 1) {
            return false;
        }
        return ::SSL_connect(ssl_) == 1;
    }

    // 读对端证书的 CN。用 CN 而不是序列号：序列号在自签证书里不可控，
    // 而 make_self_signed 的 CN 是调用方给的常量，能当身份标签用。
    std::string peer_cn() {
        if (ssl_ == nullptr) {
            return std::string();
        }
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        X509* peer = ::SSL_get1_peer_certificate(ssl_);
#else
        X509* peer = ::SSL_get_peer_certificate(ssl_);
#endif
        if (peer == nullptr) {
            return std::string();
        }
        char buf[256] = {0};
        ::X509_NAME_get_text_by_NID(::X509_get_subject_name(peer), NID_commonName, buf,
                                    sizeof(buf));
        ::X509_free(peer);
        return std::string(buf);
    }

    bool send(const std::string& req) {
        if (ssl_ == nullptr) {
            return false;
        }
        std::size_t off = 0;
        while (off < req.size()) {
            const int n = ::SSL_write(ssl_, req.data() + off, static_cast<int>(req.size() - off));
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    // 读且只读一个完整响应（按 Content-Length）。本项目的服务端不用 chunked，
    // 这里跟着它的契约走，比写一个通用实现少一半代码、也少一半出错机会。
    std::string read_one() {
        std::string buf;
        if (ssl_ == nullptr) {
            return buf;
        }
        char tmp[2048];
        std::size_t header_end = std::string::npos;
        std::size_t want       = 0;
        for (;;) {
            if (header_end == std::string::npos) {
                header_end = buf.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    header_end += 4;
                    const std::string key = "Content-Length:";
                    const std::size_t p   = buf.find(key);
                    if (p != std::string::npos && p < header_end) {
                        want = static_cast<std::size_t>(std::atoi(buf.c_str() + p + key.size()));
                    }
                }
            }
            if (header_end != std::string::npos && buf.size() >= header_end + want) {
                return buf;
            }
            const int n = ::SSL_read(ssl_, tmp, sizeof(tmp));
            if (n <= 0) {
                return buf;
            }
            buf.append(tmp, static_cast<std::size_t>(n));
        }
    }

private:
    void close() {
        if (ssl_ != nullptr) {
            ::SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (ctx_ != nullptr) {
            ::SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_     = nullptr;
    int fd_       = -1;
};

void ok_handler(const HttpRequest& req, HttpResponse& resp) {
    resp.status = 200;
    resp.body   = "tls:" + req.path;
}

}  // namespace

int main() {
    if (!tls::Available()) {
        std::printf("SKIP: 本二进制未编译 TLS（DCACHE_TLS=OFF）\n");
        return 0;
    }
    std::printf("tls backend: %s\n", tls::Backend().c_str());

    // ---- 1. 明文回归：不配 TLS 时行为必须与改造前一致 ----
    // 放在最前面，因为 HttpClient 的 TLS 开关是进程级、开了就关不掉。
    {
        HttpServer s;
        CHECK(s.Start("127.0.0.1", 0, ok_handler));
        CHECK(!s.GetMetrics().tls_enabled);

        std::string body;
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/plain", body, 2000));
        CHECK(body == "tls:/plain");
        CHECK(HttpClient::IdleConnections() > 0);  // 明文 keep-alive 照旧
        s.Stop();
        HttpClient::CloseIdleConnections();
    }

    CertFiles good{};
    CertFiles no_ip_san{};
    CertFiles other_ca{};
    CHECK(make_self_signed("localhost", true, good));
    CHECK(make_self_signed("localhost", false, no_ip_san));
    CHECK(make_self_signed("localhost", true, other_ca));  // 当作「错误的 CA」

    // ---- 2. TLS 往返 ----
    {
        tls::ServerConfig sc;
        sc.cert_file = good.cert_path;
        sc.key_file  = good.key_path;

        HttpServer s;
        CHECK(s.Start("127.0.0.1", 0, ok_handler, 4, sc));
        CHECK(s.GetMetrics().tls_enabled);

        std::string err;
        CHECK(HttpClient::EnableTls(tls::ClientConfig{good.cert_path}, err));

        // 按 IP 连接：证书的 SAN 里有 IP:127.0.0.1，走 set1_ip_asc 分支。
        std::string body;
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/over-tls", body, 3000));
        CHECK(body == "tls:/over-tls");

        // 再来一次：这条连接应当被复用（keep-alive 在 TLS 上同样生效）。
        std::string body2;
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/again", body2, 3000));
        CHECK(body2 == "tls:/again");

        // 按域名连接：走 SNI + SSL_set1_host 分支，DNS SAN 匹配。
        std::string body3;
        CHECK(HttpClient::Get("localhost", s.Port(), "/by-name", body3, 3000));
        CHECK(body3 == "tls:/by-name");

        // ---- 3. 明文客户端打 TLS 端口：不能崩，也不能被当成正常请求 ----
        const std::uint64_t before = s.GetMetrics().handshake_failures;
        const std::string raw =
            raw_exchange(s.Port(), "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(raw.find("HTTP/1.1") == std::string::npos);  // 没有明文响应

        // 服务端必须还活着：一条 TLS 请求照样能过。
        std::string body4;
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/after-garbage", body4, 3000));
        CHECK(body4 == "tls:/after-garbage");

        // 握手失败要记账：不记的话「对端没开 TLS」在监控上是隐形的。
        CHECK(s.GetMetrics().handshake_failures > before);

        s.Stop();
        HttpClient::CloseIdleConnections();
    }

    // ---- 4. 错误的 CA：握手必须失败 ----
    // 这条是整个 TLS 有没有意义的分水岭。如果它通过了，说明校验被关掉了，
    // 那么「已启用 TLS」就只是一句包装。
    {
        tls::ServerConfig sc;
        sc.cert_file = good.cert_path;
        sc.key_file  = good.key_path;

        HttpServer s;
        CHECK(s.Start("127.0.0.1", 0, ok_handler, 2, sc));

        std::string err;
        CHECK(HttpClient::EnableTls(tls::ClientConfig{other_ca.cert_path}, err));

        std::string body;
        CHECK(!HttpClient::Get("127.0.0.1", s.Port(), "/must-fail", body, 2000));

        // 服务端视角在这里**看不到**这次失败，这是 TLS 1.3 的语义，不是缺陷：
        // 服务端的 SSL_accept 发出 Finished 之后就返回成功（1-RTT），而客户端要到
        // 收到证书才发现校验不过，然后自己断开。所以 handshake_failures 不会涨。
        //
        // 这条断言原来写的是 handshake_failures >= 1，一直是靠「alert 赶在 accept
        // 之前到达」这个时序碰巧通过 —— 在 -O0 + 覆盖率插桩的构建上就红了。
        // 能确定断言的是「一条业务请求都没被处理」：客户端没发应用数据就断了。
        // 「校验确实生效」由上一条 CHECK 证明（客户端连不上）。
        CHECK(s.GetMetrics().requests_total == 0);

        s.Stop();
    }

    // ---- 5. 主机名 / IP 校验分流 ----
    // 证书只有 DNS:localhost，按 IP 连必须失败；按域名连必须成功。
    // 这一条专门盯 SSL_set1_host 与 X509_VERIFY_PARAM_set1_ip_asc 的分支。
    {
        tls::ServerConfig sc;
        sc.cert_file = no_ip_san.cert_path;
        sc.key_file  = no_ip_san.key_path;

        HttpServer s;
        CHECK(s.Start("127.0.0.1", 0, ok_handler, 2, sc));

        std::string err;
        CHECK(HttpClient::EnableTls(tls::ClientConfig{no_ip_san.cert_path}, err));

        std::string body;
        CHECK(!HttpClient::Get("127.0.0.1", s.Port(), "/ip-must-fail", body, 2000));

        std::string body2;
        CHECK(HttpClient::Get("localhost", s.Port(), "/name-ok", body2, 2000));
        CHECK(body2 == "tls:/name-ok");

        s.Stop();
        HttpClient::CloseIdleConnections();
    }

    // ---- 6. 双节点集群：转发链路整条走 TLS ----
    // 前面几条验证的是「能连上」，这一条验证的是「业务仍然正确」：
    // 一致性哈希选中 B、A 经 TLS 转发、内部 token 生效、结果回填。
    {
        tls::ServerConfig sc;
        sc.cert_file = good.cert_path;
        sc.key_file  = good.key_path;

        std::string err;
        CHECK(HttpClient::EnableTls(tls::ClientConfig{good.cert_path}, err));

        std::shared_ptr<Node> nodeA;
        std::shared_ptr<Node> nodeB;
        auto getter = [](const std::string& k) -> GetResult { return GetResult(true, "v:" + k); };

        HttpServer sA;
        HttpServer sB;
        CHECK(sA.Start(
            "127.0.0.1", 0,
            [&nodeA](const HttpRequest& req, HttpResponse& resp) {
                if (nodeA) {
                    nodeA->ServeHTTP(req, resp);
                }
            },
            4, sc));
        CHECK(sB.Start(
            "127.0.0.1", 0,
            [&nodeB](const HttpRequest& req, HttpResponse& resp) {
                if (nodeB) {
                    nodeB->ServeHTTP(req, resp);
                }
            },
            4, sc));

        const std::string a = "127.0.0.1:" + std::to_string(sA.Port());
        const std::string b = "127.0.0.1:" + std::to_string(sB.Port());

        nodeA = std::make_shared<Node>(a, 150);
        nodeB = std::make_shared<Node>(b, 150);
        nodeA->AddGroup("g", 100, getter);
        nodeB->AddGroup("g", 100, getter);

        const std::vector<std::string> peers{a, b};
        nodeA->SetPeers(peers);
        nodeB->SetPeers(peers);
        nodeA->SetAuthToken("unit-test-token");
        nodeB->SetAuthToken("unit-test-token");

        ConsistentHash ring(150);
        ring.AddNode(a);
        ring.AddNode(b);
        std::string key;
        for (int i = 0;; ++i) {
            key = "k" + std::to_string(i);
            if (ring.GetNode(key) == b) {
                break;
            }
        }

        const GetResult r = nodeA->Get("g", key);
        CHECK(r.found());
        CHECK(r.value == "v:" + key);
        CHECK(nodeB->FindGroup("g")->stats().source_fetch == 1);  // B 回源一次
        CHECK(nodeA->FindGroup("g")->stats().remote_fetch == 1);  // A 经 TLS 转发一次

        // 二次命中必须在本地，不能再走一次 TLS 转发。
        const GetResult r2 = nodeA->Get("g", key);
        CHECK(r2.found());
        CHECK(nodeA->FindGroup("g")->stats().remote_fetch == 1);

        // 两侧都一次握手失败都没有：说明整条链路是干净地走 TLS 的。
        CHECK(sA.GetMetrics().handshake_failures == 0);
        CHECK(sB.GetMetrics().handshake_failures == 0);

        sA.Stop();
        sB.Stop();
        HttpClient::CloseIdleConnections();
    }

    // ---- 7. 证书热加载：换证书不重启、失败不中断 ----
    // 这组用例要证明三件事，缺一件热加载就不成立：
    //   a) 换文件 + reload 之后，新连接拿到的是新证书；
    //   b) reload 之前建立的连接**继续可用**（旧上下文由 OpenSSL 引用计数续命）；
    //   c) reload 失败时旧证书仍在服务，节点不被打挂。
    // 第 c 条尤其重要：运维覆盖文件时读到半截 PEM 是常态，那不该是一次事故。
    {
        CertFiles gen_a{};
        CertFiles gen_b{};
        CHECK(make_self_signed("reload-a", true, gen_a));
        CHECK(make_self_signed("reload-b", true, gen_b));

        // 服务端只认这两个固定路径，内容稍后原地覆盖 —— 这正是运维的常规做法
        // （cp/mv 到同一路径，再发 SIGHUP）。
        CertFiles live{};
        live.cert_path = temp_path();
        live.key_path  = temp_path();
        CHECK(!live.cert_path.empty() && !live.key_path.empty());
        CHECK(write_file(live.cert_path, read_file(gen_a.cert_path)));
        CHECK(write_file(live.key_path, read_file(gen_a.key_path)));

        tls::ServerConfig sc;
        sc.cert_file = live.cert_path;
        sc.key_file  = live.key_path;

        HttpServer s;
        CHECK(s.Start("127.0.0.1", 0, ok_handler, 4, sc));
        CHECK(s.GetMetrics().tls_enabled);

        // 7.1 当前发的是 A
        {
            TlsProbe p;
            CHECK(p.open(s.Port()));
            CHECK(p.peer_cn() == "reload-a");
        }

        // 7.2 先建一条 keep-alive 连接，reload 之后要在这条老连接上再发请求
        TlsProbe live_conn;
        CHECK(live_conn.open(s.Port()));
        CHECK(live_conn.send("GET /before HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"));
        CHECK(live_conn.read_one().find("tls:/before") != std::string::npos);

        // 7.3 原地覆盖成 B，然后 reload
        CHECK(write_file(live.cert_path, read_file(gen_b.cert_path)));
        CHECK(write_file(live.key_path, read_file(gen_b.key_path)));
        std::string err;
        CHECK(s.ReloadTls(err));
        CHECK(s.GetMetrics().tls_reload_total == 1);
        CHECK(s.GetMetrics().tls_reload_failures == 0);

        // 7.4 新连接看到的是 B —— 这是「轮换真的生效」的唯一直接证据
        {
            TlsProbe p;
            CHECK(p.open(s.Port()));
            CHECK(p.peer_cn() == "reload-b");
        }

        // 7.5 reload 之前的老连接仍然能跑完第二个请求。
        // 如果实现里直接 SSL_CTX_free 掉旧上下文，这里会是空响应或连接被掐。
        // 顺带：这条用例在 ASan 下也是「SSL_new 会给 SSL_CTX 加引用」这个
        // 前提的验证 —— 假设不成立时这里就是一处 use-after-free。
        CHECK(live_conn.send("GET /after HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CHECK(live_conn.read_one().find("tls:/after") != std::string::npos);

        // 7.6 证书写成半截：reload 必须失败，且旧证书继续服务
        CHECK(write_file(live.cert_path, "-----BEGIN CERTIFICATE-----\nnot-a-cert\n"));
        CHECK(!s.ReloadTls(err));
        CHECK(!err.empty());
        CHECK(s.GetMetrics().tls_reload_failures == 1);
        CHECK(s.GetMetrics().tls_reload_total == 1);  // 失败不能计入成功
        {
            TlsProbe p;
            CHECK(p.open(s.Port()));  // 服务没被打挂
            CHECK(p.peer_cn() == "reload-b");
        }

        // 7.7 修好文件后能恢复，说明失败不是「把上下文弄坏了」而是「没动它」
        CHECK(write_file(live.cert_path, read_file(gen_a.cert_path)));
        CHECK(write_file(live.key_path, read_file(gen_a.key_path)));
        CHECK(s.ReloadTls(err));
        CHECK(s.GetMetrics().tls_reload_total == 2);
        {
            TlsProbe p;
            CHECK(p.open(s.Port()));
            CHECK(p.peer_cn() == "reload-a");
        }

        // 全程不该出现握手失败：热加载只换证书，不该影响任何一次握手。
        CHECK(s.GetMetrics().handshake_failures == 0);

        s.Stop();
        remove_files(live);
        remove_files(gen_a);
        remove_files(gen_b);
    }

    remove_files(good);
    remove_files(no_ip_san);
    remove_files(other_ca);

    std::printf("all TLS tests passed\n");
    return 0;
}
