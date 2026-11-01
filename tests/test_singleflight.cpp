#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "singleflight.hpp"

using namespace dcache;

int main() {
    SingleFlight<int> sf;

    // 100 并发同一 key：回源只执行一次，其余共享结果。
    std::atomic<int> calls{0};
    const int N = 100;
    std::vector<std::thread> ts;
    std::vector<int> results(N, -1);
    std::atomic<bool> start{false};  // 栅栏：确保所有线程几乎同时进入 Do（慢运行时也稳健）

    for (int i = 0; i < N; ++i) {
        ts.emplace_back([&, i]() {
            while (!start.load())
                std::this_thread::yield();
            results[i] = sf.Do("same-key", [&]() {
                calls.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 拉长并发窗口
                return 42;
            });
        });
    }
    start = true;
    for (auto& th : ts)
        th.join();

    CHECK(calls.load() == 1);  // 防击穿：只回源一次
    for (int i = 0; i < N; ++i) {
        CHECK(results[i] == 42);  // 所有人都拿到共享结果
    }
    CHECK(sf.InFlight() == 0);  // 完成后没有泄漏

    // 不同 key 互不影响。
    const int a = sf.Do("k1", [] { return 1; });
    const int b = sf.Do("k2", [] { return 2; });
    CHECK(a == 1);
    CHECK(b == 2);

    // 异常安全：leader 的回源函数抛异常 → 所有等待者应拿到异常，而非永久阻塞。
    {
        SingleFlight<int> sf2;
        std::atomic<int> throw_calls{0};
        std::atomic<int> caught{0};
        std::vector<std::thread> ts2;
        std::atomic<bool> start2{false};  // 栅栏：确保 8 线程几乎同时进入 Do

        for (int i = 0; i < 8; ++i) {
            ts2.emplace_back([&]() {
                while (!start2.load())
                    std::this_thread::yield();
                try {
                    (void)sf2.Do("boom", [&]() -> int {
                        throw_calls.fetch_add(1);
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));  // 保证并发重叠窗口
                        throw std::runtime_error("boom");
                    });
                } catch (const std::runtime_error&) {
                    caught.fetch_add(1);
                }
            });
        }
        start2 = true;
        for (auto& th : ts2)
            th.join();
        CHECK(throw_calls.load() == 1);  // 回源只执行一次
        CHECK(caught.load() == 8);       // 所有人都拿到异常，而非死锁
        CHECK(sf2.InFlight() == 0);      // 无 inflight 泄漏
    }

    return 0;
}