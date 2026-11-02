// libFuzzer 目标：JSON 编解码。
//
// 这个目标检查的核心不变式是「自己写出去的报文，自己必须能读回来」。
// 一条往返断言同时压住 JsonEscape / JsonUnescape / JsonFindString / JsonFindBool
// 四条路径，而且检查的是行为而不是「没崩」：
// 只要转义漏掉一个字符（比如 '"' 忘了转义），value 就会在往返中被截断，立刻报错。
//
// 但它有一个盲区，第十批才补上：往返断言只喂给 JsonEscape，而 JsonEscape 对非 ASCII
// 是原样透传、**从不产出 \uXXXX**。于是「\uD83D 这种代理怎么写」这条路径，
// fuzz 从构造上就走不到 —— 代理对缺陷因此活了九批。第二条不变式把原始字节
// 直接当 JSON 字符串**内容**去反转义，逼它去碰 \uD800 之类。

#include <cstddef>
#include <cstdint>
#include <string>

#include "check.hpp"
#include "json.hpp"
// 验证代码共用：跨目录总比抄两份好，抄的那份一定会在某一版里走样。
#include "../tests/utf8_check.hpp"

namespace {

void round_trip(const std::string& value, bool found) {
    const std::string encoded = dcache::EncodeResult(value, found);

    std::string decoded;
    // 故意取反：如果 DecodeResult 压根没写 found，这一步就能看出来。
    bool decoded_found = !found;

    const bool ok = dcache::DecodeResult(encoded, decoded, decoded_found);
    fuzz_check::require(ok, "EncodeResult 的输出 DecodeResult 读不回来", value);
    fuzz_check::require(decoded == value, "字符串往返后与输入不一致", value);
    fuzz_check::require(decoded_found == found, "found 布尔往返后与输入不一致", value);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    round_trip(input, true);
    round_trip(input, false);

    // 再把任意畸形报文直接灌进解析器：只要求不崩、不越界（ASan 兜底）。
    std::string v;
    bool b = false;
    (void)dcache::DecodeResult(input, v, b);
    (void)dcache::JsonFindString(input, "value", v);
    (void)dcache::JsonFindBool(input, "found", b);

    // 不变式：**输入是合法 UTF-8 时**，反转义的结果也必须是合法 UTF-8。
    //
    // 不看语义、不看具体码点，只看字节序列合法 —— 这是 decoder 对外说得清的
    // 承诺。\uD83D 单独出现、\uD800\u0041 这种假配对、截断的 \uD8，都要么合并成
    // 正确码点，要么替换成 U+FFFD，不能编出 ED A0 xx 这种「看着像 UTF-8、
    // RFC 3629 明令禁止」的字节。
    //
    // 「输入合法」这个前提不是给实现让路，而是这条函数的真实契约：
    // JsonUnescape 对非转义字节是**逐字节透传**的（缓存值可以是任意字节串，
    // 不能因为「不像文本」就被改写）。所以喂进畸形字节、输出也跟着畸形，
    // 那是调用方违反前提，不是解码器的错。
    //
    // 这条前提是第一版漏掉的：初始写法无条件断言输出合法，于是 fuzz 在
    // 「输入里带 0xFF」这类样本上立刻红——一个恒假断言比没有断言更糟，
    // 它会把真正的信号淹掉。第一版同时漏掉了它为什么会红。
    if (verify::IsValidUtf8(input)) {
        const std::string unescaped = dcache::JsonUnescape(input);
        fuzz_check::require(verify::IsValidUtf8(unescaped),
                            "输入是合法 UTF-8，JsonUnescape 却输出了非法 UTF-8", input);
    }
    return 0;
}
