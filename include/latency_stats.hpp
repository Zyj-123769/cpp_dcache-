#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dcache {

// 延迟样本集：只做两件事 —— 收样本、算分位数。
//
// 为什么单独抽出来，而不是留在 apps/bench.cpp 里：
//
//   1. 分位数是个「差一位也算对」的算法。写成一行 `samples[n * p / 100]`，
//      p=99、n=100 时取到的是第 99 个下标（即第 100 个样本），报出来的 P99
//      比真值高一格；p=100 时直接越界。这类错的表现是「数字看着很正常，只是
//      偏了一点」，混在一屏压测数字里几乎不可能被肉眼发现。抽成独立类才能给它写
//      一条会红的断言（tests/test_latency_stats.cpp）。
//
//   2. 压测工具该负责「发请求、量时间」，统计口径不该混在业务逻辑里。
//      混在一起的结果是：想换个分位数定义，得改 bench 的主循环。
//
// 口径（三条都是刻意的，换掉会得到不一样的数字）：
//
//   - **最近秩（nearest-rank），不做线性插值。** 返回的永远是某个真实观测到的
//     请求的延迟。插值会给出从没发生过的数字（P99 = 147.6 微秒），而调容量时
//     关心的是「第 99 个百分位那条请求究竟等了多久」。
//   - **只收成功请求。** 失败（超时/连接拒绝）的「延迟」是超时上限，把它混进来
//     会让 P99 变成一个假的 2000ms，而且失败越多、P99 越接近超时值——一个
//     完全由错误率驱动的「延迟」指标。失败率是另一个独立的数。
//   - **单位是微秒。** 压测报告里毫秒会把 loopback 上的差异抹平（RTT 常在
//     几十微秒量级，1ms 分辨率下全变成 0 或 1）。
class LatencyStats {
public:
    void Reserve(std::size_t count) { samples_.reserve(count); }

    void Add(std::int64_t micros) {
        samples_.push_back(micros);
        sorted_ = false;
    }

    // 把另一份样本并进来。压测是多线程各自计时，最后合并成一份再算分位数——
    // 而不是各线程各算一套再取平均：分位数不可平均，把 8 个线程的 P99 取算术
    // 平均得到的不是全局 P99。
    void Merge(const LatencyStats& other) {
        if (other.samples_.empty()) {
            return;
        }
        samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
        sorted_ = false;
    }

    std::size_t Count() const { return samples_.size(); }
    bool Empty() const { return samples_.empty(); }

    // 千分位分位数，permille ∈ [0, 1000]，超出范围会被夹住。
    //
    // 用千分位而不是百分位，是为了 P99.9 这种档位：百分位只能表达 P99，
    // 而 P99.9 恰恰是「偶发卡顿」最先露头的地方。
    //
    // 返回第 ceil(permille/1000 * n) 个样本（1-based）。空集返回 0。
    std::int64_t Permille(int permille) const;

    // 百分位的便捷包装：Percentile(99) == Permille(990)。
    std::int64_t Percentile(int percent) const;

    std::int64_t Min() const;
    std::int64_t Max() const;
    double Mean() const;

private:
    // 排序是 const 操作对外的一部分（算分位数必须先排），但调用方不该被要求
    // 记得先调 Sort()：顺序错了不报错、只算错数，正是这一批要防的那类 bug。
    // 所以排序放在 const 函数里按需做，并用 sorted_ 标记保证只排一次。
    void EnsureSorted() const;

    // mutable：排序是「为了回答查询」而做的内部整理，不改变这份数据的语义。
    mutable std::vector<std::int64_t> samples_;
    mutable bool sorted_ = true;  // 空集视为已排序
};

inline void LatencyStats::EnsureSorted() const {
    if (!sorted_) {
        std::sort(samples_.begin(), samples_.end());
        sorted_ = true;
    }
}

inline std::int64_t LatencyStats::Permille(int permille) const {
    if (samples_.empty()) {
        return 0;
    }

    const int p = permille < 0 ? 0 : (permille > 1000 ? 1000 : permille);

    EnsureSorted();

    const std::size_t n = samples_.size();
    // 最近秩：rank = ceil(p * n / 1000)，1-based。
    // 全程整数运算 —— 把「向上取整」编码进算术本身，而不是算完之后再取整。
    // 后者最常见的写法是 (int)(p * n / 1000.0)，那是**截断**：
    //   p=500、n=3     时 1.5   被截成 1   （正确 2，报的是第 1 小而不是中位数）
    //   p=999、n=1001 时 999.999 被截成 999（正确 1000，P999 正好偏一格）
    // 注意这不是「浮点不可靠」：ceil(p * n / 1000.0) 与整数式完全一致，
    // 问题是**取整那一步**太容易被写成截断。
    std::size_t rank = (static_cast<std::size_t>(p) * n + 999) / 1000;
    if (rank < 1) {
        rank = 1;  // p=0 时 ceil 算出 0，夹到第一个样本（即最小值）
    }
    if (rank > n) {
        rank = n;  // 防御：p=1000 时理论上等于 n，但仍夹一次，免得将来改公式越界
    }
    return samples_[rank - 1];
}

inline std::int64_t LatencyStats::Percentile(int percent) const {
    const int p = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    return Permille(p * 10);
}

inline std::int64_t LatencyStats::Min() const {
    if (samples_.empty()) {
        return 0;
    }
    return *std::min_element(samples_.begin(), samples_.end());
}

inline std::int64_t LatencyStats::Max() const {
    if (samples_.empty()) {
        return 0;
    }
    return *std::max_element(samples_.begin(), samples_.end());
}

inline double LatencyStats::Mean() const {
    if (samples_.empty()) {
        return 0.0;
    }
    // 累加用 double。int64 其实完全够用——1e6 个样本 × 1e4 微秒 = 1e10，
    // 离 int64 的上限（9.2e18）差着八个数量级；double 的 53 位尾数在
    // 这个规模上也远没到精度边界。所以这里不是「int64 不够、double 才够」，
    // 而是「两者都够，而这里要的精度只到 0.1 微秒」，选哪个都行。
    double total = 0.0;
    for (const std::int64_t v : samples_) {
        total += static_cast<double>(v);
    }
    return total / static_cast<double>(samples_.size());
}

}  // namespace dcache
