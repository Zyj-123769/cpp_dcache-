#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"
#include "group.hpp"
#include "http.hpp"
#include "peer_pool.hpp"

namespace dcache {

// 分布式缓存节点：持有若干 Group 与一个共享的 HttpPeerPool。
//
// 职责：
//   - 对外提供统一读取接口 Get(group, key)；
//   - ServeHTTP 把 HTTP 请求 /api/{group}/{key} 路由到对应 Group；
//   - 维护集群节点列表（一致性哈希环）。
class Node {
public:
    // self_addr 形如 "127.0.0.1:8001"。
    explicit Node(std::string self_addr, int virtual_nodes = 150);

    // 注册分组（group name 在同一 Node 内唯一），返回其指针（内存由 Node 持有）。
    Group* AddGroup(const std::string& name, std::size_t capacity, Getter getter,
                    std::uint64_t default_ttl_ms     = 0,
                    std::uint64_t penetration_ttl_ms = 30000);

    Group* FindGroup(const std::string& name);

    // 设置集群中全部节点地址（含自身）。
    void SetPeers(const std::vector<std::string>& peers);

    // 内部共享 token。配置后，仅当请求携带相同 token 才采信 X-DCache-Hops；
    // 留空则退回「来源 IP 属于 peers」的判定（只能在单机/可信内网使用）。
    void SetAuthToken(const std::string& token);
    bool HasAuthToken() const;

    // 读取 group/key（线程安全）。
    GetResult Get(const std::string& group, const std::string& key);

    // 供 HttpServer 回调：解析 /api/{group}/{key}，返回 JSON。
    // 另支持 GET /stats 返回各分组统计（命中率 / 回源次数等）。
    void ServeHTTP(const HttpRequest& req, HttpResponse& resp);

    // 汇总各 Group 的统计为 JSON 字符串。
    std::string DumpStats() const;

    // Prometheus 文本格式（GET /metrics）。
    std::string BuildMetrics() const;

    // 追加额外指标（服务端连接数、客户端连接池等）。Node 不应该知道 HttpServer 的存在，
    // 所以用钩子注入而不是直接依赖。**必须在 Start 之前设置**，运行期不再改写。
    using MetricsHook = std::function<void(std::string&)>;
    void SetMetricsHook(MetricsHook hook) { metrics_hook_ = std::move(hook); }

    const std::string& Self() const { return self_; }

private:
    std::string self_;
    std::shared_ptr<HttpPeerPool> pool_;
    std::unordered_map<std::string, std::shared_ptr<Group>> groups_;
    mutable std::mutex mtx_; // 保护 groups_

    // 携带 X-DCache-Hops 但未通过可信校验的请求数。
    // 这个计数是「有人在伪造内部头」的唯一信号，必须对外可见（/metrics），
    // 否则这类攻击在日志里完全隐形。
    std::atomic<std::uint64_t> hops_rejected_{0};

    MetricsHook metrics_hook_;
};

} // namespace dcache