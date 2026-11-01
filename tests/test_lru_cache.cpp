#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "lru_cache.hpp"

using namespace dcache;

int main() {
    // 基础 get / put
    LruCache<std::string, std::string> c(2);
    CHECK(!c.get("a").has_value());
    c.put("a", "1", 0);
    const auto va = c.get("a");
    CHECK(va.has_value() && *va == "1");

    // 更新已有 key
    c.put("a", "2", 0);
    CHECK(c.get("a").value() == "2");

    // 容量淘汰（LRU：淘汰最久未使用）
    c.put("b", "3", 0);
    c.get("a");              // a 变为最近使用
    c.put("c", "4", 0);      // 淘汰 b
    CHECK(!c.get("b").has_value());
    CHECK(c.get("a").has_value());
    CHECK(c.get("c").has_value());

    // 容量为 0：get 恒未命中、put 直接丢弃
    LruCache<int, int> z(0);
    z.put(1, 2, 0);
    CHECK(!z.get(1).has_value());
    CHECK(z.size() == 0);

    // TTL 惰性过期
    LruCache<std::string, int> t(2);
    t.put("k", 42, 50);      // 50ms TTL
    CHECK(t.get("k").has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(!t.get("k").has_value()); // 过期未命中且被惰性删除
    CHECK(t.size() == 0);

    // erase / contains
    LruCache<std::string, std::string> e(4);
    e.put("x", "1", 0);
    CHECK(e.contains("x"));
    CHECK(e.erase("x"));
    CHECK(!e.contains("x"));
    CHECK(!e.erase("x"));

    // 并发读写不崩溃、容量不越界
    LruCache<int, int> cc(100);
    std::atomic<bool> start{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j < 10000; ++j) {
                cc.put(j % 200, j, 0);
                (void)cc.get(j % 200);
            }
        });
    }
    start = true;
    for (auto& t : ts) t.join();
    CHECK(cc.size() <= 100);

    // 统计计数
    const auto st = c.stats();
    CHECK(st.hits + st.misses > 0);

    return 0;
}