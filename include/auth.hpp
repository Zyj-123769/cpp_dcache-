#pragma once

#include <cstddef>
#include <string>

namespace dcache {

// 常量时间的字符串比较。
//
// 为什么不能用 ==：std::string 的比较在第一个不同的字节处就会返回，比较耗时
// 与「前缀有多长」相关。攻击者对同一个内部接口反复发请求、统计响应时间，
// 就能一个字节一个字节地把 token 试出来（时序侧信道）。
//
// 这里的实现固定遍历 max(len(a), len(b)) 次，且不提前返回；长度不同也走完整循环，
// 避免「长度不符立刻返回」反而成为更明显的信号。
inline bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    const std::size_t n = a.size() > b.size() ? a.size() : b.size();

    // 长度不同时结果必然为 false，但差异只体现在这个累加初值上。
    unsigned char diff = (a.size() == b.size()) ? 0 : 1;

    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char ca =
            i < a.size() ? static_cast<unsigned char>(a[i]) : static_cast<unsigned char>(0);
        const unsigned char cb =
            i < b.size() ? static_cast<unsigned char>(b[i]) : static_cast<unsigned char>(0);
        diff = static_cast<unsigned char>(diff | (ca ^ cb));
    }
    return diff == 0;
}

} // namespace dcache
