#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace dcache {

// 线程安全的 LRU 缓存，可带 TTL 惰性过期。
//
// 核心结构（复用 01 项目 LRU 设计）：
//   - std::list<Node> order_                         : 头部=最近使用，尾部=最久未使用
//   - std::unordered_map<K, list::iterator> index_   : key -> 链表迭代器，O(1) 定位
//   - std::mutex mtx_                                : 保护以上结构，保证多线程读写安全
//
// TTL：条目可选携带到期时刻；get 命中时若已过期则惰性删除并视为未命中。
template <typename K, typename V>
class LruCache {
public:
    using key_type   = K;
    using value_type = V;
    using size_type  = std::size_t;

    // 命中 / 未命中计数（未命中含「命中但已过期」）。
    struct Stats {
        std::uint64_t hits   = 0;
        std::uint64_t misses = 0;
    };

    struct Node {
        key_type                              key;
        value_type                            value;
        std::chrono::steady_clock::time_point expire_at;
        bool                                  has_ttl = false;
    };

    using list_type = std::list<Node>;
    using iterator  = typename list_type::iterator;
    using map_type  = std::unordered_map<key_type, iterator>;

    // capacity == 0 表示禁用缓存：get 恒未命中、put 立即丢弃。
    explicit LruCache(size_type capacity);

    // 命中且未过期返回 value；否则返回 nullopt（惰性删除过期条目）。
    std::optional<value_type> get(const key_type& key);

    // 写入。ttl_ms == 0 表示永不过期（仅受容量淘汰约束）。
    void put(const key_type& key, const value_type& value, std::uint64_t ttl_ms);

    // 删除；返回是否删除成功。
    bool erase(const key_type& key);

    // 是否存在且未过期（不改变访问顺序）。
    bool contains(const key_type& key) const;

    // 注意：以下三个接口内部需要加锁（std::mutex::lock 可能抛 system_error），
    // 因此不能标 noexcept——一旦抛出会直接 terminate。
    size_type size() const;
    size_type capacity() const noexcept;

    // 清空缓存并清零统计。
    void clear();

    // 命中 / 未命中计数。
    Stats stats() const;

private:
    void touch(iterator it);
    void evict_lru();
    bool expired(const Node& n, std::chrono::steady_clock::time_point now) const;

    size_type           capacity_;
    mutable std::mutex  mtx_;
    list_type           order_;
    map_type            index_;
    Stats               stats_;
};

} // namespace dcache

// 模板实现必须对实例化点可见，故在声明末尾包含实现片段。
#include "../src/lru_cache.tpp"