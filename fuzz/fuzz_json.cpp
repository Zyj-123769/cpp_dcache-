// libFuzzer 目标：JSON 编解码。
//
// 这个目标检查的核心不变式是「自己写出去的报文，自己必须能读回来」。
// 一条往返断言同时压住 JsonEscape / JsonUnescape / JsonFindString / JsonFindBool
// 四条路径，而且检查的是行为而不是「没崩」：
// 只要转义漏掉一个字符（比如 '"' 忘了转义），value 就会在往返中被截断，立刻报错。

#include <cstddef>
#include <cstdint>
#include <string>

#include "check.hpp"
#include "json.hpp"

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
    return 0;
}
