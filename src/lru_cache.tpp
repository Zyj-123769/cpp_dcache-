#pragma once

namespace dcache {

template <typename K, typename V>
LruCache<K, V>::LruCache(size_type capacity) : capacity_(capacity) {}

template <typename K, typename V>
auto LruCache<K, V>::get(const key_type& key) -> std::optional<value_type> {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = index_.find(key);
    if (it == index_.end()) {
        ++stats_.misses;
        return std::nullopt;
    }

    Node& node = *it->second;
    if (expired(node, now)) {
        order_.erase(it->second);  // 惰性删除过期条目
        index_.erase(it);
        ++stats_.misses;
        return std::nullopt;
    }

    touch(it->second);
    ++stats_.hits;
    return node.value;
}

template <typename K, typename V>
void LruCache<K, V>::put(const key_type& key, const value_type& value, std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lk(mtx_);

    const auto expire_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);

    auto it = index_.find(key);
    if (it != index_.end()) {
        // 更新已有条目：更新值、过期时刻，并移到头部。
        it->second->value     = value;
        it->second->has_ttl   = (ttl_ms != 0);
        it->second->expire_at = expire_at;
        touch(it->second);
        return;
    }

    if (capacity_ == 0) {
        return;  // 禁用缓存：直接丢弃
    }

    Node node;
    node.key       = key;
    node.value     = value;
    node.has_ttl   = (ttl_ms != 0);
    node.expire_at = expire_at;

    order_.push_front(std::move(node));
    index_[key] = order_.begin();

    if (order_.size() > capacity_) {
        evict_lru();
    }
}

template <typename K, typename V>
bool LruCache<K, V>::erase(const key_type& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    order_.erase(it->second);
    index_.erase(it);
    return true;
}

template <typename K, typename V>
bool LruCache<K, V>::contains(const key_type& key) const {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    return !expired(*it->second, now);
}

template <typename K, typename V>
typename LruCache<K, V>::size_type LruCache<K, V>::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return order_.size();
}

template <typename K, typename V>
typename LruCache<K, V>::size_type LruCache<K, V>::capacity() const noexcept {
    return capacity_;
}

template <typename K, typename V>
void LruCache<K, V>::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    order_.clear();
    index_.clear();
    stats_ = Stats{};
}

template <typename K, typename V>
typename LruCache<K, V>::Stats LruCache<K, V>::stats() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return stats_;
}

template <typename K, typename V>
void LruCache<K, V>::touch(iterator it) {
    order_.splice(order_.begin(), order_, it);
}

template <typename K, typename V>
void LruCache<K, V>::evict_lru() {
    if (order_.empty()) {
        return;
    }
    index_.erase(order_.back().key);
    order_.pop_back();
}

template <typename K, typename V>
bool LruCache<K, V>::expired(const Node& n, std::chrono::steady_clock::time_point now) const {
    return n.has_ttl && now >= n.expire_at;
}

}  // namespace dcache