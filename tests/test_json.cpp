#include <string>

#include "check.hpp"
#include "json.hpp"

using namespace dcache;

int main() {
    // 转义 / 反转义往返一致（含引号、反斜杠、换行、制表符）。
    const std::string raw = "hello \"world\" \n \t \\ end";
    const std::string esc = JsonEscape(raw);
    CHECK(JsonUnescape(esc) == raw);

    // 控制字符通过 \u00XX 往返。
    const std::string ctrl("\x01\x1f", 2);
    CHECK(JsonUnescape(JsonEscape(ctrl)) == ctrl);

    // 编码 / 解码。
    std::string v;
    bool found = false;

    const std::string e1 = EncodeResult("a\"b\nc", true);
    CHECK(DecodeResult(e1, v, found));
    CHECK(found == true);
    CHECK(v == "a\"b\nc");

    const std::string e2 = EncodeResult("", false);
    CHECK(DecodeResult(e2, v, found));
    CHECK(found == false);
    CHECK(v == "");

    // 字段提取。
    const std::string obj = "{\"value\":\"x\",\"found\":false}";
    CHECK(JsonFindBool(obj, "found", found) && found == false);
    CHECK(JsonFindString(obj, "value", v) && v == "x");

    // 非法输入应安全失败。
    CHECK(!DecodeResult("not a json", v, found));

    // 回归：fuzz 撞出来的「值内部含字段名片段」。
    //
    // 旧实现用 body.find("\"found\"") 找字段，不认字符串边界。缓存值是 `a"found` 时，
    // 编码结果是 {"value":"a\"found","found":true}，其中 a\"found" 这一段的
    // 第 12~18 字节恰好是 "found"，于是扫描命中值内部：
    //   - 构造得巧（如 x","found":false,"y":"）时，读到的是值里的 false —— 命中被读成未命中；
    //   - 构造得普通时，命中后面不是冒号，直接返回 false —— 正常响应被判成畸形。
    // 两种都是线上事故，而且人工用例和四批 review 都没看出来。
    const std::string tricky[] = {
        "a\"found",  "\"found", "x\",\"found\":false,\"y\":\"", "\"value\":\"x", "value",
        "\"value\"", "a\\b",    std::string("\x01\x1f", 2)};
    const bool flags[] = {true, false};
    for (const auto& t : tricky) {
        for (bool f : flags) {
            std::string out;
            bool out_found = !f;
            CHECK(DecodeResult(EncodeResult(t, f), out, out_found));
            CHECK(out_found == f);
            CHECK(out == t);
        }
    }

    return 0;
}