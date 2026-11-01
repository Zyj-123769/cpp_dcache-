#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "auth.hpp"
#include "check.hpp"
#include "consistent_hash.hpp"
#include "group.hpp"
#include "http.hpp"
#include "http_client.hpp"
#include "http_server.hpp"
#include "node.hpp"
#include "peer_pool.hpp"

using namespace dcache;

namespace {

std::atomic<int> g_source_calls{0};

GetResult counting_getter(const std::string& key) {
    g_source_calls.fetch_add(1);
    if (key == "absent" || key.rfind("miss:", 0) == 0) {
        return GetResult(false, "");
    }
    return GetResult(true, "v:" + key);
}

// 可控的「对端」：先连续报错（模拟超时/5xx），再恢复。
// 用来验证错误结果不会被写进负缓存。
class FlakyPicker : public PeerPicker {
public:
    mutable int calls = 0;
    mutable bool fail = true;

    std::string PickPeer(const std::string&) const override { return "10.0.0.1:1"; }

    GetResult GetFromPeer(const std::string&, const std::string&, const std::string& key,
                          int) const override {
        ++calls;
        if (fail) {
            return GetResult::Error("simulated peer timeout");
        }
        return GetResult::Ok("v:" + key);
    }
};

// 原始 socket 客户端：构造 HttpClient 做不到的报文
// （发完立刻关写端、声明超大 Content-Length 等）。
// 返回 false 表示对端没给出任何响应（连接被断开）。
bool raw_exchange(std::uint16_t port, const std::string& request, std::string& response,
                  bool half_close) {
    response.clear();

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    struct sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) != 0) {
        ::close(fd);
        return false;
    }

    std::size_t off = 0;
    while (off < request.size()) {
        const ssize_t n = ::send(fd, request.data() + off, request.size() - off, 0);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    if (half_close) {
        ::shutdown(fd, SHUT_WR);  // 模拟「发完请求立刻关写端」的客户端
    }

    char tmp[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        response.append(tmp, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return !response.empty();
}

}  // namespace

// 可重复使用的原始连接：验证服务端 keep-alive 必须靠它。
// HttpClient 是同步「一问一答」，永远看不到「同一连接上的第二条请求」。
class RawConn {
public:
    ~RawConn() { Close(); }

    bool Open(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return false;
        }
        struct sockaddr_in sin;
        std::memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port   = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) != 1 ||
            ::connect(fd_, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) != 0) {
            Close();
            return false;
        }
        return true;
    }

    bool Send(const std::string& s) {
        std::size_t off = 0;
        while (off < s.size()) {
            const ssize_t n = ::send(fd_, s.data() + off, s.size() - off, 0);
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    // 读出一条完整响应（按 Content-Length 定界），多余字节留在缓冲里给下一条用。
    bool ReadResponse(std::string& out) {
        for (;;) {
            const std::size_t hpos = buf_.find("\r\n\r\n");
            if (hpos != std::string::npos) {
                const std::size_t clp = buf_.find("Content-Length: ");
                if (clp != std::string::npos && clp < hpos) {
                    const std::size_t want = static_cast<std::size_t>(
                        std::strtoul(buf_.c_str() + clp + 16, nullptr, 10));
                    const std::size_t total = hpos + 4 + want;
                    if (buf_.size() >= total) {
                        out = buf_.substr(0, total);
                        buf_.erase(0, total);
                        return true;
                    }
                }
            }
            char tmp[4096];
            const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                return false;
            }
            buf_.append(tmp, static_cast<std::size_t>(n));
        }
    }

    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    std::string buf_;
};

int main() {
    // A. 单机 Group（无 picker）：本地命中 + 回源一次 + 空值缓存防穿透。
    {
        g_source_calls = 0;
        Group g("g", 100, counting_getter);

        const GetResult r1 = g.Get("k");
        CHECK(r1.found() && r1.value == "v:k");
        CHECK(g_source_calls.load() == 1);

        const GetResult r2 = g.Get("k");
        CHECK(r2.found());
        CHECK(g_source_calls.load() == 1);  // 二次访问本地命中，未再回源

        const GetResult ra = g.Get("absent");
        CHECK(!ra.found());
        const int after_miss = g_source_calls.load();
        g.Get("absent");
        CHECK(g_source_calls.load() == after_miss);  // 空值已缓存，未再回源
    }

    // H1 回归：极小 TTL（1/2/3ms）不应触发模零 UB（ttl_with_jitter 的 span==0 路径）。
    {
        for (std::uint64_t ttl : {std::uint64_t(1), std::uint64_t(2), std::uint64_t(3)}) {
            Group tiny("tiny", 10, counting_getter, ttl);
            const GetResult r = tiny.Get("h1-key");
            CHECK(r.found());  // 不崩溃且正常返回
        }
    }

    // B. singleflight：100 并发同一 key，只回源一次。
    {
        std::atomic<int> calls{0};
        // 专用慢 getter：回源时睡 50ms 拉长并发窗口，保证 100 线程都能进入合并期。
        auto slow_getter = [&](const std::string& k) {
            calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return GetResult(true, "v:" + k);
        };

        Group g("g", 100, slow_getter);
        std::vector<std::thread> ts;
        std::atomic<int> got{0};
        std::atomic<bool> start{false};  // 栅栏：确保所有线程几乎同时进入 Get
        for (int i = 0; i < 100; ++i) {
            ts.emplace_back([&]() {
                while (!start.load())
                    std::this_thread::yield();
                if (g.Get("hot").found())
                    got.fetch_add(1);
            });
        }
        start = true;
        for (auto& th : ts)
            th.join();
        CHECK(calls.load() == 1);
        CHECK(got.load() == 100);
    }

    // C. 双节点 HTTP 转发：key 路由到 B，A 经 HTTP 取数并回填。
    {
        g_source_calls = 0;
        std::shared_ptr<Node> nodeA;
        std::shared_ptr<Node> nodeB;

        HttpServer sA, sB;
        CHECK(sA.Start("127.0.0.1", 0, [&nodeA](const HttpRequest& req, HttpResponse& resp) {
            if (nodeA)
                nodeA->ServeHTTP(req, resp);
        }));
        CHECK(sB.Start("127.0.0.1", 0, [&nodeB](const HttpRequest& req, HttpResponse& resp) {
            if (nodeB)
                nodeB->ServeHTTP(req, resp);
        }));

        const std::string a = "127.0.0.1:" + std::to_string(sA.Port());
        const std::string b = "127.0.0.1:" + std::to_string(sB.Port());

        nodeA = std::make_shared<Node>(a, 150);
        nodeB = std::make_shared<Node>(b, 150);
        nodeA->AddGroup("g", 100, counting_getter);
        nodeB->AddGroup("g", 100, counting_getter);

        const std::vector<std::string> peers{a, b};
        nodeA->SetPeers(peers);
        nodeB->SetPeers(peers);

        // 找一个确定性路由到 B 的 key。
        ConsistentHash ring(150);
        ring.AddNode(a);
        ring.AddNode(b);
        std::string key;
        for (int i = 0;; ++i) {
            key = "key" + std::to_string(i);
            if (ring.GetNode(key) == b)
                break;
        }

        const GetResult r1 = nodeA->Get("g", key);
        CHECK(r1.found());
        CHECK(nodeB->FindGroup("g")->stats().source_fetch == 1);  // B 回源一次
        CHECK(nodeA->FindGroup("g")->stats().remote_fetch == 1);  // A 转发一次

        const GetResult r2 = nodeA->Get("g", key);
        CHECK(r2.found());
        CHECK(nodeA->FindGroup("g")->stats().remote_fetch == 1);  // 二次本地命中，未再转发
        CHECK(nodeA->FindGroup("g")->stats().local_hit >= 1);

        sA.Stop();
        sB.Stop();
    }

    // D. P0-1 回归：handler 抛异常必须变成 500，而不是让工作线程 terminate 掉进程。
    {
        HttpServer s;
        CHECK(s.Start(
            "127.0.0.1", 0,
            [](const HttpRequest&, HttpResponse&) { throw std::runtime_error("boom"); }, 2));

        std::string body;
        // 5xx → HttpClient::Get 返回 false。能走到这一步本身就说明进程还活着。
        CHECK(!HttpClient::Get("127.0.0.1", s.Port(), "/boom", body, 2000));
        s.Stop();
    }

    // E. P0-5 回归：Stop 之后再次 Start，请求必须仍能被处理（shutdown_ 需复位）。
    {
        std::atomic<int> served{0};
        auto handler = [&served](const HttpRequest&, HttpResponse& resp) {
            resp.status = 200;
            resp.body   = "ok";
            served.fetch_add(1);
        };

        HttpServer s;
        CHECK(s.Start("127.0.0.1", 0, handler, 2));
        s.Stop();

        CHECK(s.Start("127.0.0.1", 0, handler, 2));  // 重启
        std::string body;
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/ping", body, 2000));
        CHECK(body == "ok");
        CHECK(served.load() == 1);
        s.Stop();
    }

    // F. S6 回归：移除节点后，剩余节点必须仍保有完整的虚拟节点数。
    //    旧实现按 hash 直接 erase，哈希冲突时会连别人的位置一起删掉。
    {
        ConsistentHash ring(150);
        ring.AddNode("127.0.0.1:8001");
        ring.AddNode("127.0.0.1:8002");
        CHECK(ring.RingSize() == 300);

        ring.RemoveNode("127.0.0.1:8002");
        CHECK(ring.RingSize() == 150);  // 只剩 A 的 150 个，一个不少
        CHECK(ring.Contains("127.0.0.1:8001"));
        CHECK(!ring.Contains("127.0.0.1:8002"));
    }

    // G. P0-4 回归：对端报错必须原样上抛，且绝不写入负缓存。
    //    旧实现把「超时/5xx/报文坏」压成 found=false，再按未命中缓存 30s——
    //    于是对端抖一下，用户连续 30s 看到「数据不存在」，而第二次 Get 连对端都不会再问。
    {
        auto picker = std::make_shared<FlakyPicker>();
        Group g("g", 100, counting_getter);
        g.SetPeers(picker);

        const GetResult r1 = g.Get("k");
        CHECK(r1.is_error());
        CHECK(picker->calls == 1);

        const GetResult r2 = g.Get("k");
        CHECK(r2.is_error());
        CHECK(picker->calls == 2);  // 错误没进缓存，必须重新去问对端

        picker->fail       = false;
        const GetResult r3 = g.Get("k");
        CHECK(r3.ok());
        CHECK(r3.value == "v:k");
        CHECK(picker->calls == 3);
    }

    // H. 错误响应体：节点对 kError 返回 503 且带 error 字段，而不是 200+found=false。
    {
        HttpServer s;
        Node node("127.0.0.1:1", 150);
        node.AddGroup("g", 10,
                      [](const std::string&) { return GetResult::Error("source is down"); });
        CHECK(s.Start("127.0.0.1", 0, [&node](const HttpRequest& req, HttpResponse& resp) {
            node.ServeHTTP(req, resp);
        }));

        std::string body;
        int status = 0;
        CHECK(!HttpClient::Get("127.0.0.1", s.Port(), "/api/g/k", body, 2000, {}, &status));
        CHECK(status == 503);
        s.Stop();
    }

    // I. 连接层回归：「发完请求立刻关写端」的客户端必须仍能拿到响应。
    //    这类连接的同批事件是 EPOLLIN|EPOLLHUP，旧实现先判 HUP 就直接关连接，
    //    缓冲区里那条完整请求被丢掉，客户端只看到连接被重置。
    {
        HttpServer s;
        CHECK(s.Start(
            "127.0.0.1", 0,
            [](const HttpRequest&, HttpResponse& resp) {
                resp.status = 200;
                resp.body   = "ok";
            },
            2));

        std::string resp;
        CHECK(raw_exchange(s.Port(), "GET /x HTTP/1.1\r\nHost: a\r\n\r\n", resp, true));
        CHECK(resp.find("200") != std::string::npos);
        CHECK(resp.find("ok") != std::string::npos);
        s.Stop();
    }

    // J. 连接层回归：声明超大 Content-Length 的请求必须立刻断开，而不是挂在那里等。
    //    读缓冲在 64KB 封顶，这种请求永远「收不全」，旧实现会让连接一直占着位置。
    {
        HttpServer s;
        CHECK(s.Start(
            "127.0.0.1", 0,
            [](const HttpRequest&, HttpResponse& resp) {
                resp.status = 200;
                resp.body   = "should not happen";
            },
            2));

        std::string resp;
        CHECK(!raw_exchange(s.Port(),
                            "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 100000000\r\n\r\n",
                            resp, true));
        CHECK(resp.empty());  // 没有任何响应体 = 服务端直接断开
        s.Stop();
    }

    // K. keep-alive 回归：同一连接上的第二条请求必须仍被服务。
    //    服务端处理完把 fd 交还事件循环重新挂回 epoll；旧实现一律 Connection: close，
    //    这里第二条请求会直接读到 EOF。
    {
        std::atomic<int> served{0};
        HttpServer s;
        CHECK(s.Start(
            "127.0.0.1", 0,
            [&served](const HttpRequest&, HttpResponse& resp) {
                resp.status = 200;
                resp.body   = "ok";
                served.fetch_add(1);
            },
            2));

        RawConn c;
        CHECK(c.Open(s.Port()));

        CHECK(c.Send("GET /a HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n"));
        std::string r1;
        CHECK(c.ReadResponse(r1));
        CHECK(r1.find("200") != std::string::npos);
        CHECK(r1.find("Connection: keep-alive") != std::string::npos);

        CHECK(c.Send("GET /b HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n"));
        std::string r2;
        CHECK(c.ReadResponse(r2));
        CHECK(r2.find("200") != std::string::npos);
        CHECK(served.load() == 2);

        s.Stop();
    }

    // L. 内部 token：伪造 X-DCache-Hops 但 token 不对，必须按 hops=0 处理并记账。
    //    这里刻意让 remote_ip 是 127.0.0.1——旧的 IP 判定会直接放行，
    //    只有 token 校验才能挡住同机的伪造者。
    {
        Node node("127.0.0.1:1", 150);
        node.AddGroup("g", 10, counting_getter);
        node.SetAuthToken("s3cret");

        auto request = [&](const char* token) {
            HttpRequest req;
            req.method                   = "GET";
            req.path                     = "/api/g/k";
            req.remote_ip                = "127.0.0.1";
            req.headers["X-DCache-Hops"] = "2";
            if (token != nullptr) {
                req.headers["X-DCache-Token"] = token;
            }
            HttpResponse resp;
            node.ServeHTTP(req, resp);
            return resp;
        };

        auto rejected = [&]() -> long {
            HttpRequest m;
            m.method = "GET";
            m.path   = "/metrics";
            HttpResponse mr;
            node.ServeHTTP(m, mr);
            const std::string needle = "dcache_hops_rejected_total ";
            const std::size_t p      = mr.body.find(needle);
            if (p == std::string::npos) {
                return -1;
            }
            return std::strtol(mr.body.c_str() + p + needle.size(), nullptr, 10);
        };

        CHECK(rejected() == 0);
        CHECK(request("wrong").status == 200);  // 请求本身仍正常受理，只是不信那个头
        CHECK(rejected() == 1);

        request(nullptr);  // 连 token 都不带
        CHECK(rejected() == 2);

        request("s3cret");  // 正确 token
        CHECK(rejected() == 2);
    }

    // M. /metrics 输出必须符合 Prometheus 文本格式的关键约定。
    {
        Node node("127.0.0.1:1", 150);
        node.AddGroup("scores", 10, counting_getter);
        node.Get("scores", "k1");  // 未命中 + 回源
        node.Get("scores", "k1");  // 命中

        HttpRequest m;
        m.method = "GET";
        m.path   = "/metrics";
        HttpResponse resp;
        node.ServeHTTP(m, resp);

        CHECK(resp.status == 200);
        auto ct = resp.headers.find("Content-Type");
        CHECK(ct != resp.headers.end() && ct->second.find("text/plain") != std::string::npos);
        CHECK(resp.body.find("# TYPE dcache_group_local_hit_total counter") != std::string::npos);
        CHECK(resp.body.find("dcache_group_local_hit_total{group=\"scores\"} 1") !=
              std::string::npos);
        CHECK(resp.body.find("dcache_group_source_fetch_total{group=\"scores\"} 1") !=
              std::string::npos);
        CHECK(resp.body.find("dcache_group_cache_size{group=\"scores\"} 1") != std::string::npos);
        CHECK(resp.body.find("dcache_hops_rejected_total 0") != std::string::npos);
    }

    // N. token 比较的行为正确性（常量时间是对实现的要求，这里只验语义）。
    {
        CHECK(ConstantTimeEquals("abc", "abc"));
        CHECK(!ConstantTimeEquals("abc", "abd"));
        CHECK(!ConstantTimeEquals("abc", "ab"));
        CHECK(!ConstantTimeEquals("", "x"));
        CHECK(ConstantTimeEquals("", ""));
    }

    // O. 客户端连接复用：连续两次转发后池中应留有空闲连接；关掉开关后不再入池。
    {
        HttpServer s;
        CHECK(s.Start(
            "127.0.0.1", 0,
            [](const HttpRequest&, HttpResponse& resp) {
                resp.status = 200;
                resp.body   = "ok";
            },
            2));

        HttpClient::CloseIdleConnections();
        std::string body;
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/a", body, 2000));
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/b", body, 2000));
        CHECK(HttpClient::IdleConnections() >= 1);

        HttpClient::SetKeepAlive(false);
        HttpClient::CloseIdleConnections();
        CHECK(HttpClient::Get("127.0.0.1", s.Port(), "/c", body, 2000));
        CHECK(HttpClient::IdleConnections() == 0);
        HttpClient::SetKeepAlive(true);

        s.Stop();
        // 服务端已停：池中指向它的连接必须清掉，否则后续用例会取到死连接。
        HttpClient::CloseIdleConnections();
    }

    return 0;
}