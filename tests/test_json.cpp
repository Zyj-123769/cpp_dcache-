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

    return 0;
}