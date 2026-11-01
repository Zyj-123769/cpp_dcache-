#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "common.hpp"
#include "lru_cache.hpp"
#include "singleflight.hpp"

namespace dcache {

class PeerPicker;

// 单个缓存分组（对应 groupcache 的 Group）。
//
// 读取链路：
//   1. 本地 LRU 命中 → 直接返回；
//   2. 未命中 → singleflight 合并并发（防击穿）→ 回源：
//        a. 无 PeerPicker 或该 key 由本机负责 → 调用 getter 回源；
//        b. 由远端负责 → 经 PeerPicker 转发到负责节点取数；
//   3. 回填本地缓存：命中用默认 TTL，未命中用短 TTL（防穿透）；
//   4. 出错（GetStatus::kError）直接返回调用方，不回填缓存——「没问到」不等于「不存在」。
class Group {
public:
    struct Stats {
        std::uint64_t local_hit    = 0;
        std::uint64_t local_miss   = 0;
        std::uint64_t source_fetch = 0;  // 本机回源次数
        std::uint64_t remote_fetch = 0;  // 转发到远端次数
        std::uint64_t error_count  = 0;  // 回源/转发失败次数（结果未缓存）
        std::uint64_t cache_size   = 0;
    };

    Group(std::string name, std::size_t capacity, Getter getter, std::uint64_t default_ttl_ms = 0,
          std::uint64_t penetration_ttl_ms = 30000);

    const std::string& Name() const { return name_; }

    // 注入节点选择器（可选；不注入则退化为单机缓存）。
    void SetPeers(const std::shared_ptr<PeerPicker>& picker) { picker_ = picker; }

    // 转发最多经过的跳数（0=起点/自身）。超过则退化本机回源，防止环配置不一致时无限转发。
    static constexpr int kMaxForwardHops = 2;

    // 完整读取链路（线程安全）。hops=已转发跳数（外部调用用默认 0）。
    GetResult Get(const std::string& key);
    GetResult Get(const std::string& key, int hops);

    Stats stats() const;

private:
    GetResult load(const std::string& key, int hops);
    GetResult load_local(const std::string& key);
    std::uint64_t ttl_with_jitter(std::uint64_t base_ms, const std::string& key) const;

    std::string name_;
    Getter getter_;
    std::uint64_t default_ttl_ms_;
    std::uint64_t penetration_ttl_ms_;

    LruCache<std::string, GetResult> cache_;
    SingleFlight<GetResult> flight_;
    std::shared_ptr<PeerPicker> picker_;

    mutable std::mutex stats_mtx_;
    Stats stats_;
};

}  // namespace dcache