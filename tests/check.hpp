#pragma once

#include <cstdio>

// 极简测试断言：失败打印位置并返回 1（仅用于返回 int 的函数，通常是 main）。
// 注意：用单行宏定义，避免行尾续接符 `\` 在跨平台换行差异下触发告警。
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAILED: %s\n  at %s:%d\n", #cond, __FILE__, __LINE__); \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                          \
    do {                                                                                        \
        const auto _va = (a);                                                                   \
        const auto _vb = (b);                                                                   \
        if (!(_va == _vb)) {                                                                    \
            std::fprintf(stderr, "FAILED: %s == %s\n  at %s:%d\n", #a, #b, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)