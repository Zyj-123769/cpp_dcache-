#pragma once

#include <functional>
#include <string>
#include <utility>

namespace dcache {

// 缓存的 value 统一用 std::string 表示。
using Value = std::string;

// 一次读取的结果状态。
//
// 三态是这套缓存最关键的契约之一。早期实现只有一个 bool found，把「确定不存在」
// 和「没问到」（超时、连接拒绝、对端 5xx、报文损坏）都压成 found=false，结果：
//   1. 对端抖一下，负缓存就把那条并不存在的结论记住 30s，用户连续看到「数据不存在」；
//   2. 调用方也分不清该返回 404 还是 503，监控上永远看不出后端在故障。
enum class GetStatus {
    kOk,       // 数据存在，value 有效
    kNotFound, // 数据确定不存在（可安全做负缓存，防穿透）
    kError     // 没能得到答案，value 无意义，错误信息见 error
};

// 一次读取的结果。
struct GetResult {
    GetStatus   status = GetStatus::kNotFound;
    Value       value;
    std::string error; // 仅 kError 有意义，用于日志与响应体诊断

    GetResult() = default;

    GetResult(GetStatus s, Value v = Value{}, std::string err = std::string{})
        : status(s), value(std::move(v)), error(std::move(err)) {}

    // 兼容旧写法：GetResult(true, v) 等价 Ok，GetResult(false) 等价 NotFound。
    // 注意没有 GetResult(false, ...) 表达「出错」的通道——错误必须显式用 Error()。
    GetResult(bool found, Value v = Value{})
        : status(found ? GetStatus::kOk : GetStatus::kNotFound), value(std::move(v)) {}

    bool ok() const { return status == GetStatus::kOk; }
    bool found() const { return status == GetStatus::kOk; }
    bool not_found() const { return status == GetStatus::kNotFound; }
    bool is_error() const { return status == GetStatus::kError; }

    // 只有「确定存在」和「确定不存在」才允许写入缓存。
    // 错误结果一旦落缓存，就等于把一次瞬时故障固化成一段时间内的错误答案。
    bool should_cache() const { return status != GetStatus::kError; }

    static GetResult Ok(Value v) { return GetResult(GetStatus::kOk, std::move(v)); }
    static GetResult NotFound() { return GetResult(GetStatus::kNotFound); }
    static GetResult Error(std::string msg) {
        return GetResult(GetStatus::kError, Value{}, std::move(msg));
    }
};

// 回源函数签名：从数据源读取 key。
// 单机模式下直接来自 mock/真实数据源；分布式模式下仅由「负责节点」调用。
using Getter = std::function<GetResult(const std::string& key)>;

} // namespace dcache
