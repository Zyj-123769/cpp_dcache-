// 慢客户端不能独占工作线程。
//
// 缺陷本身：响应一直是由工作线程阻塞写完的（fd 设成阻塞 + SO_SNDTIMEO 2 秒）。
// 于是「只发请求、不读响应」的客户端能把一个 worker 按在 send() 上整整两秒。
// worker_count=4 时，四条这样的 TCP 连接就能让整个节点不响应任何请求——
// 攻击成本是四个 socket 和零字节读取，比 slowloris 还便宜（slowloris 至少要
// 慢慢发字节，这里连字节都不用发完）。
//
// 这组用例要钉住三件事，每一件都必须能在修复前变红：
//   1) 有人占着不读时，另一个正常请求仍然立刻被服务（核心断言）；
//   2) 大响应分多次写出时字节不多不少（写偏移量的记账正确）；
//   3) 一直不读的客户端最终会被回收——否则「改成异步写」只是把 DoS 从
//      「耗尽工作线程」换成「耗尽内存」，问题并没有消失。
//
// 为什么用「时间」当断言而不用别的手段：这里要证明的性质本来就是
// 「延迟不随他人的慢而增长」，没有别的东西可以观测。为了不引入随机失败，
// 三处都留了很宽的余量（见各处的注释），而不是卡在临界值上。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "check.hpp"
#include "http_server.hpp"

using namespace dcache;

namespace {

// 大响应体：必须大到内核发送缓冲区塞不下。
// 客户端把接收窗口压到 4KB，服务端 sndbuf 的自动调优也就涨不起来；
// 8MB 在任何常见的 tcp_wmem 上限下都必然出现「send 写不完」。
// 曾经想过用 1MB——太小了，sndbuf 自动调优能整个吞下，测试会假绿。
constexpr std::size_t kBigBody = 8u * 1024u * 1024u;

// 中等大小：小到客户端一次读得完，又大到一条 TCP 段装不下，
// 从而迫使服务端走「写一部分 -> EAGAIN -> 下一轮接着写」的路径。
constexpr std::size_t kMidBody = 1u * 1024u * 1024u;

// 「正常请求」的响应体。故意小，保证它能不能被服务只取决于 worker 是否空闲，
// 不取决于这条响应本身好不好写。
constexpr const char* kSmallBody = "ok";

int Connect(std::uint16_t port, int rcvbuf) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (rcvbuf > 0) {
        // 显式设了 SO_RCVBUF 就关掉了接收窗口的自动调优，窗口会稳定压在
        // 这个值附近——这才让「服务端写不完」变成确定事件而不是看运气。
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }

    struct sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool SendAll(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// 在 ms 毫秒内尽量收，到点就带着「已经收到的字节」返回。
// 不用带 SO_RCVTIMEO 的阻塞 recv：那条路把「超时」和「失败」混成一个返回值，
// 而这里恰恰要区分「一个字都没收到」和「收了一半」。
std::string ReadWithin(int fd, int ms) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    char tmp[8192];
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return out;  // 到点
        }
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd p;
        p.fd      = fd;
        p.events  = POLLIN;
        p.revents = 0;
        const int r = ::poll(&p, 1, left);
        if (r == 0) {
            return out;  // 到点
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return out;
        }
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return out;  // 对端关闭
        }
        out.append(tmp, static_cast<std::size_t>(n));
    }
}

}  // namespace

int main() {
    std::string big_body(kBigBody, 'x');  // 'x' 便于在失败时一眼看出是不是响应体
    std::string mid_body(kMidBody, 'x');

    HttpServer srv;
    const bool started = srv.Start(
        "127.0.0.1", 0,
        [&big_body, &mid_body](const HttpRequest& req, HttpResponse& resp) {
            resp.headers["Content-Type"] = "text/plain";
            if (req.path == "/big") {
                resp.body = big_body;
            } else if (req.path == "/mid") {
                resp.body = mid_body;
            } else {
                resp.body = kSmallBody;
            }
        },
        /*worker_count=*/1);
    CHECK(started);

    const std::uint16_t port = srv.Port();

    // 1) 慢客户端占着不读时，正常请求必须立刻被服务。
    //
    // worker_count=1 是这个用例的关键：只要唯一的 worker 被按在 send() 上，
    // 第二个请求连「进 handler」的机会都没有，压根不是慢一点的问题。
    {
        const int slow = Connect(port, 4096);
        CHECK(slow >= 0);
        CHECK(SendAll(slow, "GET /big HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n"));

        // 让事件循环先把这条请求交给 worker 并进入写响应阶段。
        // 100ms 对 loopback 上的 accept+parse+入队是数量级的冗余；
        // 而修复前 worker 会在 send() 上停 2000ms，慢的这 100ms 不影响结论。
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const int fast = Connect(port, 0);
        CHECK(fast >= 0);
        CHECK(SendAll(fast, "GET /small HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"));

        // 只量「收响应」这一段：/small 的响应体只有一个字节，唯一可能让它变慢的
        // 就是服务端还没来得及处理它。
        const auto started     = std::chrono::steady_clock::now();
        const std::string resp = ReadWithin(fast, 1500);
        const long long cost =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();

        // 修复前这里是空的：worker 正卡在 /big 的 send() 里，
        // /small 在队列里排到 2000ms 之后才可能有人取。
        if (resp.empty()) {
            std::fprintf(stderr,
                         "  另一条连接在 %lld ms 内一个字节都没收到：\n"
                         "  唯一的 worker 被 /big 的阻塞写占住了。\n",
                         cost);
        }
        CHECK(!resp.empty());
        CHECK(resp.rfind("HTTP/1.1 200", 0) == 0);
        CHECK(resp.find(kSmallBody) != std::string::npos);

        // 顺带把「慢」量化出来。1500ms 的门槛远宽于正常值（loopback 上是个位数
        // 毫秒），又远窄于修复前的 2000ms 停摆——两边都不贴边。
        std::fprintf(stderr, "  [/small] %lld ms\n", cost);

        ::close(slow);
        ::close(fast);
    }

    // 2) 大响应分多次写出时，字节必须不多不少。
    //
    // 这一条盯的是「改成非阻塞写」最容易出的错：写到一半 EAGAIN，下次从新的
    // 偏移量接着写。偏移量算错的表现有两种，都不能放过——
    // 少写（客户端看到的 Content-Length 对不上）和重复写（内容错位）。
    {
        const int fd = Connect(port, 0);  // 这次正常读，1MB 一次读完不费劲
        CHECK(fd >= 0);
        CHECK(SendAll(fd, "GET /mid HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"));

        const std::string resp = ReadWithin(fd, 10000);
        ::close(fd);

        const std::size_t hpos = resp.find("\r\n\r\n");
        CHECK(hpos != std::string::npos);

        const std::size_t clp = resp.find("Content-Length: ");
        CHECK(clp != std::string::npos && clp < hpos);
        const std::size_t declared =
            static_cast<std::size_t>(std::strtoull(resp.c_str() + clp + 16, nullptr, 10));
        CHECK(declared == resp.size() - (hpos + 4));

        // 全部是 'x'：偏移量算错时这里会混进头部字节或出现空洞。
        CHECK(resp.find_first_not_of('x', hpos + 4) == std::string::npos);
    }

    // 3) 一直不读的客户端必须被回收。
    //
    // 前提：上面第 2 组用的是 Connection: close，连接已经没了。
    // 这里重新开一条只发不读的连接，等它被写超时清掉。
    // 写超时沿用原来的 2 秒（见 kSendTimeoutMs），扫描周期 1 秒，
    // 所以给 8 秒的观察窗口；再长就说明它根本不会被回收。
    {
        const int stuck = Connect(port, 4096);
        CHECK(stuck >= 0);
        CHECK(SendAll(stuck, "GET /big HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n"));
        // 一个字都不读。连接会被内核缓冲吸掉若干字节，然后停在那儿。

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);

        bool reclaimed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (srv.GetMetrics().active_conns == 0) {
                reclaimed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!reclaimed) {
            std::fprintf(stderr, "  8 秒后仍有 %zu 条连接挂着：不读响应的客户端没有被回收。\n",
                         srv.GetMetrics().active_conns);
        }
        CHECK(reclaimed);
        ::close(stuck);
    }

    srv.Stop();
    std::fprintf(stderr, "test_worker_isolation ok\n");
    return 0;
}
