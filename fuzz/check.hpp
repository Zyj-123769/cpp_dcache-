#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

// fuzz 专用的断言。
//
// 为什么不用 assert：assert 在 NDEBUG（Release）下会被整个吃掉，
// 造出一条「看着在检查、其实什么都没检查」的门禁——本项目的指标断言
// 已经栽过一次同样的跟头（见 docs/REVIEW_REPORT 第十一节）。
// fuzz 的价值全在断言上：没有不变式，libFuzzer 就只是个更快的随机程序。
namespace fuzz_check {

// 打印可读的输入，二进制的部分转成 \xNN。截断到 160 字节，
// 否则一条 64KB 的畸形报文会把 CI 日志刷掉。
inline void print_bytes(const std::string& s) {
    std::size_t shown = 0;
    for (unsigned char c : s) {
        if (shown++ >= 160) {
            std::fputs("...", stderr);
            break;
        }
        if (c >= 0x20 && c < 0x7f) {
            std::fputc(static_cast<int>(c), stderr);
        } else {
            std::fprintf(stderr, "\\x%02x", c);
        }
    }
}

inline void require(bool cond, const char* what, const std::string& input) {
    if (cond) {
        return;
    }
    std::fprintf(stderr, "\n[fuzz] 不变式被破坏: %s\n  输入(%zu 字节): ", what, input.size());
    print_bytes(input);
    std::fputc('\n', stderr);
    // abort 让 libFuzzer 把当前输入存成 crash-<sha>，直接就是可复现的最小样本。
    std::abort();
}

}  // namespace fuzz_check
