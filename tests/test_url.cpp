#include <string>

#include "check.hpp"
#include "url.hpp"

using namespace dcache;

int main() {
    // 编码 / 解码往返一致（含空格、斜杠、查询符、非 ASCII）。
    const std::string samples[] = {
        "hello", "a/b", "a b", "a?b=c", "key#1", "a%20b", "\xe4\xb8\xad\xe6\x96\x87"};
    for (const auto& s : samples) {
        CHECK(UrlDecodeComponent(UrlEncodeComponent(s)) == s);
    }

    // 特殊字符确实被转义为期望的百分号编码。
    CHECK(UrlEncodeComponent("a/b") == "a%2Fb");
    CHECK(UrlEncodeComponent("a b") == "a%20b");
    CHECK(UrlEncodeComponent("a?b") == "a%3Fb");

    // 非法序列应原样保留，不越界、不崩溃。
    CHECK(UrlDecodeComponent("100%") == "100%");
    CHECK(UrlDecodeComponent("%zz") == "%zz");

    return 0;
}