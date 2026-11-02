#pragma once

#include <cstddef>
#include <string>

// 最小 UTF-8 合法性检查：只判「字节序列是否合法」，不判语义（不做 NFC、不管非字符）。
//
// 为什么不用库：这条断言要说的就是「输出的字节必须合法」，用系统 locale 或第三方
// 库去判会把被测行为和环境绑定在一起。规则按 RFC 3629：
//   - 拒绝过度编码（C0/C1 开头、E0 80..9F、F0 80..8F）
//   - 拒绝代理区（ED A0..BF）—— U+D800..U+DFFF 不允许出现在 UTF-8 字节流里
//   - 拒绝超出 U+10FFFF（F4 90.. 以及 F5.. 开头）
//   - 拒绝孤立续字节与截断序列
//
// 放在独立的头里而不是随便哪个 .cpp，是因为 fuzz 目标也要用同一条不变式；
// 验证代码抄两份就会在某一版里悄悄走样。
namespace verify {

inline bool IsValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);

        // 首个续字节的合法区间。默认 80..BF，过度编码与代理区靠收窄它挡掉。
        std::size_t extra = 0;
        unsigned char lo  = 0x80;
        unsigned char hi  = 0xBF;

        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if (c == 0xE0) {
            extra = 2;
            lo    = 0xA0;  // E0 80..9F 是过度编码
        } else if (c >= 0xE1 && c <= 0xEC) {
            extra = 2;
        } else if (c == 0xED) {
            extra = 2;
            hi    = 0x9F;  // ED A0..BF 是 U+D800..U+DFFF，代理区
        } else if (c >= 0xEE && c <= 0xEF) {
            extra = 2;
        } else if (c == 0xF0) {
            extra = 3;
            lo    = 0x90;  // F0 80..8F 是过度编码
        } else if (c >= 0xF1 && c <= 0xF3) {
            extra = 3;
        } else if (c == 0xF4) {
            extra = 3;
            hi    = 0x8F;  // F4 90.. 已超过 U+10FFFF
        } else {
            return false;  // 80..C1（孤立续字节 / 过度编码）、F5..FF
        }

        if (i + extra >= s.size()) {
            return false;  // 序列被截断
        }
        const unsigned char first = static_cast<unsigned char>(s[i + 1]);
        if (first < lo || first > hi) {
            return false;
        }
        for (std::size_t k = 2; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if (cc < 0x80 || cc > 0xBF) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

}  // namespace verify
