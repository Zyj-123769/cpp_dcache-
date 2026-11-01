// HTTP 请求解析器的单元测试（解析器已从 http_server.cpp 抽成 src/http_parser.cpp）。
//
// 为什么要有这层测试：test_node 走的是「起真服务端 + 真 socket」，
// 覆盖不到几条关键分支 —— 声明体过大、负数 Content-Length 这类，
// 集成测试要么根本发不出去，要么只能证明「连接被关了」。
// 直接调用解析器可以把这些边界逐个钉死。

#include <string>

#include "check.hpp"
#include "http_parser.hpp"

using namespace dcache;

namespace {

std::string with_body(const std::string& body) {
    return "POST /k HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

}  // namespace

int main() {
    // 1. 完整 GET：字段、头部、consumed 全部要对得上。
    {
        const std::string raw =
            "GET /api/scores/k1 HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest(raw, req, consumed), static_cast<int>(kParseOk));
        CHECK(req.method == "GET");
        CHECK(req.path == "/api/scores/k1");
        CHECK(req.body.empty());
        CHECK_EQ(consumed, raw.size());
        CHECK(req.headers.at("Connection") == "keep-alive");
    }

    // 2. 只有请求行、没有结束空行：必须等更多数据，不能判成功也不能判非法。
    {
        const std::string raw = "GET / HTTP/1.1\r\nHost: x\r\n";
        HttpRequest req;
        std::size_t consumed = 99;  // 故意给个非 0，检查失败时会不会被清掉
        CHECK_EQ(ParseRequest(raw, req, consumed), static_cast<int>(kParseIncomplete));
        CHECK_EQ(consumed, std::size_t{0});
    }

    // 3. body 分两次到达：半包先说「不够」，补齐后 body 要一字不差。
    {
        const std::string full = with_body("hello");
        const std::string part = full.substr(0, full.size() - 2);
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest(part, req, consumed), static_cast<int>(kParseIncomplete));
        CHECK_EQ(ParseRequest(full, req, consumed), static_cast<int>(kParseOk));
        CHECK(req.body == "hello");
        CHECK_EQ(consumed, full.size());
    }

    // 4. 请求行缺第二个空格：非法报文。
    {
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest("GET/nope\r\n\r\n", req, consumed),
                 static_cast<int>(kParseMalformed));
    }

    // 5. 负数 Content-Length：非法（std::string::substr 收到负数会爆）。
    {
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest("GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", req, consumed),
                 static_cast<int>(kParseMalformed));
    }

    // 6. 声明体超过上限：必须立刻判「过大」，而不是挂着等一个永远收不全的 body。
    //    这条分支集成测试覆盖不到 —— 发 64KB 以上的声明再断开，服务端只会记一次 drop。
    {
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest("POST /k HTTP/1.1\r\nContent-Length: 70000\r\n\r\n", req, consumed),
                 static_cast<int>(kParseBodyTooLarge));
    }

    // 7. 非数字 Content-Length：strtol 退化成 0，按「无 body」处理。
    //    这不是理想行为（严格实现该判 400），但它是当前契约，写清楚了才不会被人当 bug 改回去。
    {
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest("GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", req, consumed),
                 static_cast<int>(kParseOk));
        CHECK(req.body.empty());
    }

    // 8. pipelining：两条请求粘在一起时，consumed 只能覆盖第一条。
    //    服务端靠这个值判断「缓冲区里还有东西」，取多了会串包。
    {
        const std::string one = "GET /a HTTP/1.1\r\n\r\n";
        const std::string two = "GET /b HTTP/1.1\r\n\r\n";
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest(one + two, req, consumed), static_cast<int>(kParseOk));
        CHECK(req.path == "/a");
        CHECK_EQ(consumed, one.size());
    }

    // 9. 没有任何头部：合法。
    {
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest("GET / HTTP/1.1\r\n\r\n", req, consumed), static_cast<int>(kParseOk));
        CHECK(req.headers.empty());
    }

    // 10. 缺冒号的头部行被忽略，不能因此崩掉或污染已有字段。
    {
        HttpRequest req;
        std::size_t consumed = 0;
        CHECK_EQ(ParseRequest("GET / HTTP/1.1\r\nbroken-line\r\nHost: x\r\n\r\n", req, consumed),
                 static_cast<int>(kParseOk));
        CHECK(req.headers.count("broken-line") == 0);
        CHECK(req.headers.at("Host") == "x");
    }

    return 0;
}
