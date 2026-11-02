// LatencyStats（分位数计算）的断言。
//
// 这个测试存在的理由很具体：分位数算错一格，报出来的数字**依然合理**。
// 压测报告里 P99 是 99 还是 100 微秒，没人能靠肉眼判断对错，只能靠这里钉死。
// 所以下面每一个用例都对着一种具体的写法错误：
//
//   组 1  空集 / 单样本 / 全相同      —— 边界与除零
//   组 2  1..100，P99 必须正好是 99   —— 挡住「下标少减 1」（0-based 混用）
//   组 3  n=3 的 P50 必须是 3         —— 挡住「用 floor 代替 ceiling」
//   组 4  乱序输入                    —— 挡住「忘了排序」
//   组 5  查询后再 Add                —— 挡住「Add 没把有序标记清掉」
//   组 6  合并                        —— 挡住「各线程各自算再取平均」
//   组 7  越界参数被夹住              —— 挡住 p=1000 时越界访问

#include <cstdint>
#include <cstdio>

#include "check.hpp"
#include "latency_stats.hpp"

using namespace dcache;

int main() {
    // ---- 组 1：边界 ----
    {
        LatencyStats empty;
        // 空集不能崩、不能除零。返回 0 是个约定值，调用方据此判断「没有样本」。
        CHECK(empty.Empty());
        CHECK_EQ(empty.Count(), 0U);
        CHECK_EQ(empty.Permille(500), 0);
        CHECK_EQ(empty.Percentile(99), 0);
        CHECK_EQ(empty.Min(), 0);
        CHECK_EQ(empty.Max(), 0);
        CHECK(empty.Mean() == 0.0);
    }
    {
        LatencyStats one;
        one.Add(42);
        // 只有一个样本时，所有分位数都等于它。这一组防的是「n=1 时算出下标 -1」。
        CHECK_EQ(one.Permille(0), 42);
        CHECK_EQ(one.Permille(500), 42);
        CHECK_EQ(one.Permille(999), 42);
        CHECK_EQ(one.Permille(1000), 42);
        CHECK(one.Mean() == 42.0);
    }
    {
        LatencyStats same;
        for (int i = 0; i < 100; ++i) {
            same.Add(7);
        }
        // 全相同时排序与否不影响结果，但边界夹取若写错就会越界（ASan 兜底）。
        CHECK_EQ(same.Permille(0), 7);
        CHECK_EQ(same.Permille(999), 7);
        CHECK_EQ(same.Permille(1000), 7);
    }

    // ---- 组 2：1..100 —— 挡住「下标少减 1」 ----
    //
    // 这是整批里最关键的一条。写成 `samples[n * p / 100]`（0-based 当 1-based 用）：
    //   P99 → samples[99] → 第 100 个样本 → 报 100（真值 99）
    //   P100 → samples[100] → 越界
    // 两种错都很安静：一个偏一格，一个只在恰好撞到页尾时才被 ASan 抓到。
    {
        LatencyStats hundred;
        for (int i = 1; i <= 100; ++i) {
            hundred.Add(i);
        }
        CHECK_EQ(hundred.Count(), 100U);
        CHECK_EQ(hundred.Permille(0), 1);
        CHECK_EQ(hundred.Percentile(50), 50);
        CHECK_EQ(hundred.Percentile(90), 90);
        CHECK_EQ(hundred.Percentile(99), 99);
        CHECK_EQ(hundred.Permille(999), 100);  // ceil(99.9) = 100 → 第 100 个
        CHECK_EQ(hundred.Percentile(100), 100);
        CHECK_EQ(hundred.Min(), 1);
        CHECK_EQ(hundred.Max(), 100);
        // 均值必须走独立路径算，不能拿 P50 顶替（这里两者恰好接近，但 1..100 的
        // 真均值是 50.5，而 P50 是整数样本 50）。
        CHECK(hundred.Mean() > 50.4 && hundred.Mean() < 50.6);
    }

    // ---- 组 3：n=3 的 P50 —— 挡住 floor ----
    //
    // 3 个样本、P50：ceil(0.5*3) = ceil(1.5) = 2 → 第 2 小 = 3。
    // 用 floor 会得 1 → 第 1 小 = 1。差一格，但一个是「中位数」一个是「最小值」，
    // 语义完全不同。
    {
        LatencyStats three;
        three.Add(5);
        three.Add(1);
        three.Add(3);

        CHECK_EQ(three.Percentile(50), 3);
        CHECK_EQ(three.Permille(0), 1);
        // ceil(0.9*3)=ceil(2.7)=3 → 最大
        CHECK_EQ(three.Percentile(90), 5);
    }

    // ---- 组 4：乱序输入 ----
    //
    // 实际样本天然无序（每个请求的时间不会单调）。这里故意倒序喂进去，
    // 挡住「忘了排序，直接按插入顺序取下标」。
    {
        LatencyStats desc;
        for (int i = 100; i >= 1; --i) {
            desc.Add(i);
        }
        CHECK_EQ(desc.Permille(0), 1);
        CHECK_EQ(desc.Percentile(50), 50);
        CHECK_EQ(desc.Percentile(99), 99);
        CHECK_EQ(desc.Permille(1000), 100);
    }

    // ---- 组 5：查询之后再 Add ----
    //
    // 排序是惰性的（按需做一次并记标记）。如果 Add() 忘了把标记清掉，
    // 新样本会插在有序区间的末尾，后续分位数会算错——而且只在「先查后加」
    // 的顺序下发生。压测里这个顺序很常见（边跑边打点），所以必须钉住。
    {
        LatencyStats lazy;
        lazy.Add(10);
        lazy.Add(20);
        CHECK_EQ(lazy.Percentile(50), 10);  // 触发第一次排序

        lazy.Add(1);  // 比现有样本都小
        CHECK_EQ(lazy.Count(), 3U);
        CHECK_EQ(lazy.Permille(0), 1);      // 新样本进了正确的位置
        CHECK_EQ(lazy.Percentile(50), 10);  // 第 2 小
        CHECK_EQ(lazy.Permille(1000), 20);
    }

    // ---- 组 6：合并 ----
    //
    // 多线程各自计时、最后合并。分位数不可平均：两份各 100 个样本的 P99 取
    // 算术平均，不等于 200 个样本的 P99。所以这里钉住的是「先合并再算」。
    {
        LatencyStats a;
        LatencyStats b;
        for (int i = 1; i <= 3; ++i) {
            a.Add(i);
        }
        for (int i = 4; i <= 6; ++i) {
            b.Add(i);
        }

        a.Merge(b);
        CHECK_EQ(a.Count(), 6U);
        CHECK_EQ(a.Permille(0), 1);
        CHECK_EQ(a.Percentile(50), 3);  // ceil(3.0) = 3 → 第 3 小
        CHECK_EQ(a.Permille(1000), 6);

        // 合并空集不能改变任何东西（压测里某个线程分到 0 个请求是可能的）。
        LatencyStats empty;
        a.Merge(empty);
        CHECK_EQ(a.Count(), 6U);
    }

    // ---- 组 7：越界参数 ----
    //
    // 调用方传 -1 或 5000 时，必须夹到 [0,1000] 而不是越界访问。
    {
        LatencyStats s;
        for (int i = 1; i <= 10; ++i) {
            s.Add(i);
        }
        CHECK_EQ(s.Permille(-1), 1);
        CHECK_EQ(s.Permille(5000), 10);
        CHECK_EQ(s.Percentile(-1), 1);
        CHECK_EQ(s.Percentile(200), 10);

        // 单调性：0..1000 扫一遍，任何一个点倒退都说明排序或秩计算有问题。
        std::int64_t prev = s.Permille(0);
        for (int p = 0; p <= 1000; ++p) {
            const std::int64_t cur = s.Permille(p);
            if (cur < prev) {
                std::fprintf(stderr, "FAILED: Permille 不单调，p=%d 时 %lld < %lld\n", p,
                             static_cast<long long>(cur), static_cast<long long>(prev));
                return 1;
            }
            prev = cur;
        }
    }

    std::fprintf(stderr, "test_latency_stats: all checks passed\n");
    return 0;
}
