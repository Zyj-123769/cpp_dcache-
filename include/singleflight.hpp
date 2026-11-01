#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dcache {

// singleflight：合并同一 key 的并发调用，只有第一个执行 fn，其余阻塞并共享结果。
//
// 用于防缓存击穿——热点 key 过期瞬间大量并发未命中时，把 N 次回源压成 1 次，
// 避免后端数据源被瞬时打垮。
template <typename R>
class SingleFlight {
public:
    SingleFlight() = default;
    SingleFlight(const SingleFlight&) = delete;
    SingleFlight& operator=(const SingleFlight&) = delete;

    // 同一 key 并发调用只有一个真正执行 fn；其余等待并返回共享结果。
    // 调用完成后立即删除 map 条目，避免 inflight 条目泄漏。
    template <typename Fn>
    R Do(const std::string& key, Fn&& fn) {
        std::shared_ptr<Call> call;
        bool leader = false;

        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = calls_.find(key);
            if (it != calls_.end()) {
                call = it->second;
            } else {
                call = std::make_shared<Call>();
                calls_[key] = call;
                leader = true;
            }
        }

        if (leader) {
            std::exception_ptr error;
            try {
                call->value = fn();   // 真正的回源只会在这里执行一次
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lk(call->mtx);
                call->error = error;
                call->done  = true;
            }
            call->cv.notify_all();

            {
                std::lock_guard<std::mutex> lk(mtx_);
                calls_.erase(key);
            }

            if (error) {
                std::rethrow_exception(error); // leader 自身抛出异常
            }
            return call->value;
        } else {
            std::unique_lock<std::mutex> lk(call->mtx);
            call->cv.wait(lk, [&] { return call->done; });
            if (call->error) {
                std::rethrow_exception(call->error); // 等待者共享异常而非死锁
            }
            return call->value;
        }
    }

    // 当前进行中的调用数量（主要用于测试 / 观测）。
    std::size_t InFlight() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return calls_.size();
    }

private:
    struct Call {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done  = false;
        R                       value;
        std::exception_ptr      error;
    };

    mutable std::mutex                               mtx_;
    std::unordered_map<std::string, std::shared_ptr<Call>> calls_;
};

} // namespace dcache