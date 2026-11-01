#include "node.hpp"

#include <cstdio>
#include <cstdlib>

#include "json.hpp"
#include "url.hpp"

namespace dcache {

Node::Node(std::string self_addr, int virtual_nodes)
    : self_(std::move(self_addr)),
      pool_(std::make_shared<HttpPeerPool>(self_, virtual_nodes)) {}

Group* Node::AddGroup(const std::string& name, std::size_t capacity, Getter getter,
                      std::uint64_t default_ttl_ms, std::uint64_t penetration_ttl_ms) {
    auto g = std::make_shared<Group>(name, capacity, std::move(getter),
                                     default_ttl_ms, penetration_ttl_ms);
    g->SetPeers(pool_);

    std::lock_guard<std::mutex> lk(mtx_);
    groups_[name] = g;
    return g.get();
}

Group* Node::FindGroup(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = groups_.find(name);
    return it == groups_.end() ? nullptr : it->second.get();
}

void Node::SetPeers(const std::vector<std::string>& peers) {
    pool_->SetPeers(peers);
}

GetResult Node::Get(const std::string& group, const std::string& key) {
    Group* g = FindGroup(group);
    if (g == nullptr) {
        return GetResult(false, "");
    }
    return g->Get(key);
}

void Node::ServeHTTP(const HttpRequest& req, HttpResponse& resp) {
    resp.status = 200;
    resp.headers["Content-Type"] = "application/json";

    if (req.method != "GET") {
        resp.status = 405;
        resp.headers["Allow"] = "GET";
        resp.body   = "{\"error\":\"method not allowed\"}";
        return;
    }

    if (req.path == "/stats") {
        resp.body = DumpStats();
        return;
    }

    const std::string prefix = "/api/";
    if (req.path.compare(0, prefix.size(), prefix) != 0) {
        resp.status = 404;
        resp.body   = "{\"error\":\"not found\"}";
        return;
    }

    const std::string rest = req.path.substr(prefix.size()); // "group/key"
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
        resp.status = 400;
        resp.body   = "{\"error\":\"bad path\"}";
        return;
    }

    // URL 解码：peer 转发时对 group/key 做过百分号编码，这里还原。
    const std::string group = UrlDecodeComponent(rest.substr(0, slash));
    const std::string key   = UrlDecodeComponent(rest.substr(slash + 1));

    Group* g = FindGroup(group);
    if (g == nullptr) {
        resp.status = 404;
        resp.body   = "{\"error\":\"group not found\"}";
        return;
    }

    // 转发跳数只信任来自集群节点的请求。
    //
    // 若直接采信任意请求的 X-DCache-Hops，外部客户端发一个 "2" 就能让本节点判定
    // hops >= kMaxForwardHops 而退化为本机回源：既绕过了按 key 的一致性哈希分片，
    // 又把全网流量压到数据源，还能把不属于本节点的 key 塞进本地缓存。
    int hops = 0;
    if (pool_->IsTrustedHost(req.remote_ip)) {
        auto hit = req.headers.find("X-DCache-Hops");
        if (hit != req.headers.end()) {
            const int parsed = std::atoi(hit->second.c_str());
            hops = parsed < 0 ? 0
                 : (parsed > Group::kMaxForwardHops ? Group::kMaxForwardHops : parsed);
        }
    }

    const GetResult r = g->Get(key, hops);

    // 三种状态映射成三种响应，核心是「没问到」绝不能被包装成「不存在」：
    //   命中   → 200 {"value":..,"found":true}
    //   不存在 → 200 {"value":"","found":false}（保留既有节点间协议，负缓存依赖它）
    //   出错   → 503 {"error":..}（对端/客户端据此判定为 error，不会写负缓存）
    switch (r.status) {
        case GetStatus::kOk:
            resp.body = EncodeResult(r.value, true);
            break;
        case GetStatus::kNotFound:
            resp.body = EncodeResult(Value{}, false);
            break;
        case GetStatus::kError:
            resp.status = 503;
            resp.body   = "{\"error\":\"" +
                          JsonEscape(r.error.empty() ? std::string("upstream error") : r.error) +
                          "\"}";
            break;
    }
}

std::string Node::DumpStats() const {
    std::vector<std::pair<std::string, Group::Stats>> snapshots;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshots.reserve(groups_.size());
        for (const auto& kv : groups_) {
            snapshots.emplace_back(kv.first, kv.second->stats());
        }
    }

    std::string out = "{";
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto& kv = snapshots[i];
        const Group::Stats& s = kv.second;
        const std::uint64_t denom = s.local_hit + s.local_miss;
        const double hit_rate = denom ? static_cast<double>(s.local_hit) / denom : 0.0;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", hit_rate);

        if (i > 0) out += ",";
        out += "\"" + JsonEscape(kv.first) + "\":{";
        out += "\"local_hit\":" + std::to_string(s.local_hit);
        out += ",\"local_miss\":" + std::to_string(s.local_miss);
        out += ",\"hit_rate\":" + std::string(buf);
        out += ",\"source_fetch\":" + std::to_string(s.source_fetch);
        out += ",\"remote_fetch\":" + std::to_string(s.remote_fetch);
        out += ",\"error_count\":" + std::to_string(s.error_count);
        out += ",\"cache_size\":" + std::to_string(s.cache_size);
        out += "}";
    }
    out += "}";
    return out;
}

} // namespace dcache