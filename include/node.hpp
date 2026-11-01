#pragma once

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

    // 读取 group/key（线程安全）。
    GetResult Get(const std::string& group, const std::string& key);

    // 供 HttpServer 回调：解析 /api/{group}/{key}，返回 JSON。
    // 另支持 GET /stats 返回各分组统计（命中率 / 回源次数等）。
    void ServeHTTP(const HttpRequest& req, HttpResponse& resp);

    // 汇总各 Group 的统计为 JSON 字符串。
    std::string DumpStats() const;

    const std::string& Self() const { return self_; }

private:
    std::string self_;
    std::shared_ptr<HttpPeerPool> pool_;
    std::unordered_map<std::string, std::shared_ptr<Group>> groups_;
    mutable std::mutex mtx_; // 保护 groups_
};

} // namespace dcache