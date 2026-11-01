// libFuzzer 目标：URL 百分号编解码。
//
// 三条不变式，都是「行为」层面的：
//   1. 往返一致：任意字节串编码后再解码必须与原串逐字节相同；
//   2. 编码结果只含 RFC 3986 非保留字符或 %XX；
//   3. 解码只会变短或等长（非法序列按 1:1 原样保留）。
// 第 2 条能抓住「忘了转义某个字符」——它在往返测试里未必暴露；
// 第 3 条能挡住「把 % 当多字节前缀去越界读」的写法。

#include <cstddef>
#include <cstdint>
#include <string>

#include "check.hpp"
#include "url.hpp"

namespace {

bool is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    const std::string encoded = dcache::UrlEncodeComponent(input);

    fuzz_check::require(dcache::UrlDecodeComponent(encoded) == input,
                        "UrlEncode/UrlDecode 往返不一致", input);

    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (is_unreserved(c)) {
            continue;
        }
        const bool well_formed =
            c == '%' && i + 2 < encoded.size() && is_hex(encoded[i + 1]) && is_hex(encoded[i + 2]);
        fuzz_check::require(well_formed, "编码结果里出现了未转义字符", input);
        i += 2;
    }

    fuzz_check::require(dcache::UrlDecodeComponent(input).size() <= input.size(),
                        "解码结果比输入更长", input);
    return 0;
}
