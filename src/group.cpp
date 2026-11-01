#include "group.hpp"

#include "hash.hpp"
#include "peer_pool.hpp"

namespace dcache {

Group::Group(std::string name, std::size_t capacity, Getter getter, std::uint64_t default_ttl_ms,
             std::uint64_t penetration_ttl_ms)
    : name_(std::move(name)),
      getter_(std::move(getter)),
      default_ttl_ms_(default_ttl_ms),
      penetration_ttl_ms_(penetration_ttl_ms),
      cache_(capacity) {}

GetResult Group::Get(const std::string& key) {
    return Get(key, 0);
}

GetResult Group::Get(const std::string& key, int hops) {
    auto cached = cache_.get(key);
    if (cached) {
        {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            ++stats_.local_hit;
        }
        return *cached;
    }

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.local_miss;
    }

    // 同一 key 并发未命中只回源一次，其余等待共享结果。
    return flight_.Do(key, [&] { return load(key, hops); });
}

GetResult Group::load(const std::string& key, int hops) {
    if (!picker_) {
        return load_local(key);
    }

    std::string peer = picker_->PickPeer(key);
    if (peer.empty() || hops >= kMaxForwardHops) {
        // 自身负责；或已达转发上限（环配置不一致时的兜底），退化为本机回源，避免无限转发。
        return load_local(key);
    }

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.remote_fetch;
    }

    GetResult r = picker_->GetFromPeer(peer, name_, key, hops + 1);

    if (r.is_error()) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.error_count;
    }

    // 回填本地：命中用默认 TTL，未命中用短 TTL（防穿透），出错则一律不缓存。
    // 这里必须用 should_cache() 而不是「非 found 就当不存在」：对端超时/5xx 只是
    // 「这次没问到」，若按未命中处理，30s 的负缓存会把一次瞬时故障放大成错误答案。
    if (r.should_cache()) {
        cache_.put(key, r, ttl_with_jitter(r.ok() ? default_ttl_ms_ : penetration_ttl_ms_, key));
    }
    return r;
}

GetResult Group::load_local(const std::string& key) {
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.source_fetch;
    }

    GetResult r = getter_(key);

    if (r.is_error()) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.error_count;
    }

    if (r.should_cache()) {
        cache_.put(key, r, ttl_with_jitter(r.ok() ? default_ttl_ms_ : penetration_ttl_ms_, key));
    }
    return r;
}

std::uint64_t Group::ttl_with_jitter(std::uint64_t base_ms, const std::string& key) const {
    if (base_ms == 0) {
        return 0;
    }
    // 过期时间加随机抖动（±25%），破坏大量 key 同时过期的雪崩场景。
    const std::uint64_t span = (base_ms / 4) * 2;  // 抖动区间宽度 = base/2
    if (span == 0) {
        return base_ms;  // 极小 TTL 无法抖动，直接返回，避免模零 UB
    }
    const std::uint32_t h = murmur3_32(key);
    const std::int64_t delta =
        static_cast<std::int64_t>(h % span) - static_cast<std::int64_t>(span / 2);
    std::int64_t ttl = static_cast<std::int64_t>(base_ms) + delta;
    if (ttl < 1) {
        ttl = 1;
    }
    return static_cast<std::uint64_t>(ttl);
}

Group::Stats Group::stats() const {
    Stats s;
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        s = stats_;
    }
    s.cache_size = cache_.size();
    return s;
}

}  // namespace dcache