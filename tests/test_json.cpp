#include <string>

#include "check.hpp"
#include "json.hpp"
#include "utf8_check.hpp"

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

    // ── 代理对（surrogate pair）──────────────────────────────────────────
    //
    // JSON 里的 \uXXXX 转义的是一个 **UTF-16 码元**，不是码点。BMP 外的字符
    // （emoji、部分生僻字）在报文里是一对代理：😀 = U+1F600 = \uD83D\uDE00。
    //
    // 旧实现把每个 \uXXXX 独立当码点编码，于是 0xD83D 走「三字节」分支，编出
    // ED A0 BD —— 这是 CESU-8 式的写法，**不是合法 UTF-8**：RFC 3629 明确禁止
    // 把 U+D800..U+DFFF 编进字节流。对端收到的是乱码，下游 JSON 解析器通常
    // 直接判报文非法，一个带 emoji 的 value 就能把「命中」变成 503。
    //
    // 这条缺陷活了九批，原因不是没人看 json.cpp，而是**没有任何一条断言能红**：
    //   - 四批人工 review 都只看到「\u 分支存在」；
    //   - fuzz 的不变式是「自己编码的自己解得回来」，而 JsonEscape 对非 ASCII
    //     原样透传、从不产出 \uXXXX，这条路径它从构造上就走不到；
    //   - ASan 管的是越界，不管字节序列合不合法。
    // 所以先补不变式（结果必须是合法 UTF-8），再补具体向量。
    //
    // 期望字节集中写在这里：U+1F600 😀 = F0 9F 98 80，U+1F389 🎉 = F0 9F 8E 89，
    // U+FFFD = EF BF BD。写成一堆十六进制转义会让断言本身没法读。
    const std::string emoji            = "\xF0\x9F\x98\x80";
    const std::string party            = "\xF0\x9F\x8E\x89";
    const std::string replacement_char = "\xEF\xBF\xBD";

    CHECK(JsonUnescape("\\ud83d\\ude00") == emoji);
    CHECK(JsonUnescape("\\uD83D\\uDE00") == emoji);  // 大写十六进制同样有效
    CHECK(JsonUnescape("a\\ud83d\\ude00b") == "a" + emoji + "b");
    // 连续两对：合并时的下标推进写错，最容易在这里串位。
    CHECK(JsonUnescape("\\ud83c\\udf89\\ud83d\\ude00") == party + emoji);

    // 孤立代理没有「另一半」，按 JSON 生态的通行做法替换成 U+FFFD。
    // 关键不是替换成什么字符，而是**绝不能编出 ED A0 xx**。
    CHECK(JsonUnescape("\\ud800") == replacement_char);  // 高代理单独出现
    CHECK(JsonUnescape("\\udfff") == replacement_char);  // 高代理区上界
    CHECK(JsonUnescape("\\udc00") == replacement_char);  // 低代理单独出现
    // 高代理后面跟的不是 \u 转义：高代理无效，后续字符必须原样保留。
    const std::string lone_then_plain =
        "\\ud800"
        "A";
    CHECK(JsonUnescape(lone_then_plain) == replacement_char + "A");
    // 高代理后面跟了 \u，但那个 \u 不是低代理：两个码元各自处理，不能吞掉后者。
    CHECK(JsonUnescape("\\ud800\\u0041") == replacement_char + "A");
    CHECK(JsonUnescape("\\udc00\\ud83d") == replacement_char + replacement_char);

    // 边界两侧必须照旧：0xD7FF 与 0xE000 都不是代理，仍应编成三字节。
    // 这条防的是「修得太狠」——把代理区两边的正常字符一起替换掉。
    CHECK(JsonUnescape("\\ud7ff") == "\xED\x9F\xBF");
    CHECK(JsonUnescape("\\ue000") == "\xEE\x80\x80");
    CHECK(JsonUnescape("\\u4e2d") == "\xE4\xB8\xAD");  // 中：普通 BMP 字符

    // 截断的第二个转义（"\ud83d\u"）：高代理只能当孤立处理，且**不得越界读**。
    // 越界由 ASan 兜底；这里只要求输出仍是合法 UTF-8。
    CHECK(verify::IsValidUtf8(JsonUnescape("\\ud83d\\u")));
    CHECK(verify::IsValidUtf8(JsonUnescape("\\ud83d\\uD8")));
    CHECK(verify::IsValidUtf8(JsonUnescape("\\ud83d\\uZZZZ")));

    // 端到端：对端（或别的语言写的 encoder）发来的报文里带代理对。
    // 这条才是线上真正会走的路径——DecodeResult 是节点间唯一的解码入口。
    std::string escaped_value;
    bool escaped_found = false;
    CHECK(DecodeResult("{\"value\":\"\\ud83d\\ude00\",\"found\":true}", escaped_value,
                       escaped_found));
    CHECK(escaped_found == true);
    CHECK(escaped_value == emoji);

    // 不变式收口：手边所有畸形转义解码一遍，结果都必须是合法 UTF-8。
    // 这一句是「以后再来改 \u 的处理」时的保险绳。
    const std::string malformed[] = {"\\u",       "\\u0",           "\\u00",         "\\u004",
                                     "\\ud83d",   "\\ude00",        "\\uffff",       "\\u0000",
                                     "\\\\ud83d", "\\ud83d\\ud83d", "\\ud83d\\u0041"};
    std::size_t invalid           = 0;
    for (const auto& m : malformed) {
        if (!verify::IsValidUtf8(JsonUnescape(m))) {
            ++invalid;
        }
    }
    CHECK_EQ(invalid, 0U);

    // 反向契约：非转义字节必须**逐字节透传**，不能因为「不像合法文本」就改写。
    // 缓存值可以是任意字节串，把这一层做成「顺手做 UTF-8 校验并修复」会悄悄
    // 改掉用户数据——那比留一个乱码更糟，因为它是静默的。
    //
    // 这条也是上面那条 fuzz 不变式必须带「输入合法」前提的原因：透传是设计，
    // 所以「畸形输入 → 畸形输出」是调用方违反前提，不是 decoder 的缺陷。
    const std::string raw_bytes("\xFF\x01\x80", 3);
    CHECK(JsonUnescape(raw_bytes) == raw_bytes);
    CHECK(JsonUnescape(JsonEscape(raw_bytes)) == raw_bytes);  // 0x01 走 \u0001 往返

    return 0;
}