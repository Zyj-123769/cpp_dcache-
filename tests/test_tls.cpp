#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
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
        CHECK(s.GetMetrics().handshake_failures >= 1);

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

    remove_files(good);
    remove_files(no_ip_san);
    remove_files(other_ca);

    std::printf("all TLS tests passed\n");
    return 0;
}
