#include "node.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "json.hpp"
#include "url.hpp"

namespace dcache {

namespace {

// Prometheus 文本格式的标签值转义。只有反斜杠、双引号、换行需要处理，
// 但漏掉任意一个都会让整条指标被 Prometheus 判为格式错误而丢弃。
std::string PromLabelEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

void AppendHelpType(std::string& out, const char* name, const char* type, const char* help) {
    out += "# HELP ";
    out += name;
    out += " ";
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += " ";
    out += type;
    out += "\n";
}

// 指标定义表：名字 / 类型 / 说明 / 对应的 Stats 字段。
// 用表驱动而不是逐个手写，是为了让「新增一个指标」只改一行——
// 手写六遍 out += "dcache_group_xxx{group=..." 这种代码，漏一个也不会有任何报错。
struct GroupMetricDef {
    const char* name;
    const char* type;
    const char* help;
    std::uint64_t Group::Stats::* field;
};

}  // namespace

Node::Node(std::string self_addr, int virtual_nodes)
    : self_(std::move(self_addr)), pool_(std::make_shared<HttpPeerPool>(self_, virtual_nodes)) {}

Group* Node::AddGroup(const std::string& name, std::size_t capacity, Getter getter,
                      std::uint64_t default_ttl_ms, std::uint64_t penetration_ttl_ms) {
    auto g = std::make_shared<Group>(name, capacity, std::move(getter), default_ttl_ms,
                                     penetration_ttl_ms);
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

void Node::SetAuthToken(const std::string& token) {
    pool_->SetAuthToken(token);
}

bool Node::HasAuthToken() const {
    return !pool_->AuthToken().empty();
}

GetResult Node::Get(const std::string& group, const std::string& key) {
    Group* g = FindGroup(group);
    if (g == nullptr) {
        return GetResult(false, "");
    }
    return g->Get(key);
}

void Node::ServeHTTP(const HttpRequest& req, HttpResponse& resp) {
    resp.status                  = 200;
    resp.headers["Content-Type"] = "application/json";

    if (req.method != "GET") {
        resp.status           = 405;
        resp.headers["Allow"] = "GET";
        resp.body             = "{\"error\":\"method not allowed\"}";
        return;
    }

    if (req.path == "/metrics") {
        // Prometheus 抓取端点。Content-Type 必须带 version=0.0.4，
        // 否则部分版本的 Prometheus 会按旧格式解析并报错。
        resp.headers["Content-Type"] = "text/plain; version=0.0.4; charset=utf-8";
        resp.body                    = BuildMetrics();
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

    const std::string rest  = req.path.substr(prefix.size());  // "group/key"
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

    // 转发跳数只信任来自集群内部的请求。
    //
    // 若直接采信任意请求的 X-DCache-Hops，外部客户端发一个 "2" 就能让本节点判定
    // hops >= kMaxForwardHops 而退化为本机回源：既绕过了按 key 的一致性哈希分片，
    // 又把全网流量压到数据源，还能把不属于本节点的 key 塞进本地缓存。
    //
    // 判据的优先级：配置了 token 就只认 token；没配才退回「来源 IP 属于 peers」。
    int hops     = 0;
    auto hops_it = req.headers.find("X-DCache-Hops");
    if (hops_it != req.headers.end()) {
        std::string token;
        auto token_it = req.headers.find("X-DCache-Token");
        if (token_it != req.headers.end()) {
            token = token_it->second;
        }

        if (pool_->IsTrustedRequest(req.remote_ip, token)) {
            // 用 strtol 而不是 atoi：atoi 溢出是未定义行为，而这个头的值来自网络。
            //
            // 说实话这一行原本也不会出事：下面的夹取会把任何结果压进
            // [0, kMaxForwardHops]，包括 UB 之后那个「不知道是什么」的返回值。
            // 但「靠下游夹住上游的未定义行为」不该是设计——真正的防线应该是
            // 这一行自己就没有 UB，夹取只负责业务语义。
            char* end          = nullptr;
            const long parsed  = std::strtol(hops_it->second.c_str(), &end, 10);
            const bool numeric = (end != hops_it->second.c_str()) && (*end == '\0');
            if (!numeric || parsed < 0) {
                hops = 0;
            } else if (parsed > Group::kMaxForwardHops) {
                hops = Group::kMaxForwardHops;
            } else {
                hops = static_cast<int>(parsed);
            }
        } else {
            // 有人带着内部语义头来但过不了校验：按 hops=0 处理（等价外部客户端），
            // 同时记账。这类伪造尝试在访问日志里看不出任何异常，只能靠计数器暴露。
            hops_rejected_.fetch_add(1, std::memory_order_relaxed);
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
                      JsonEscape(r.error.empty() ? std::string("upstream error") : r.error) + "\"}";
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
        const auto& kv            = snapshots[i];
        const Group::Stats& s     = kv.second;
        const std::uint64_t denom = s.local_hit + s.local_miss;
        const double hit_rate     = denom ? static_cast<double>(s.local_hit) / denom : 0.0;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", hit_rate);

        if (i > 0)
            out += ",";
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

std::string Node::BuildMetrics() const {
    std::vector<std::pair<std::string, Group::Stats>> snapshots;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snapshots.reserve(groups_.size());
        for (const auto& kv : groups_) {
            snapshots.emplace_back(kv.first, kv.second->stats());
        }
    }

    static const GroupMetricDef kGroupMetrics[] = {
        {"dcache_group_local_hit_total", "counter", "本地 LRU 命中次数", &Group::Stats::local_hit},
        {"dcache_group_local_miss_total", "counter", "本地 LRU 未命中次数",
         &Group::Stats::local_miss},
        {"dcache_group_source_fetch_total", "counter", "本机回源次数", &Group::Stats::source_fetch},
        {"dcache_group_remote_fetch_total", "counter", "转发到对端的次数",
         &Group::Stats::remote_fetch},
        {"dcache_group_error_total", "counter", "回源或转发失败次数（结果不入缓存）",
         &Group::Stats::error_count},
        {"dcache_group_cache_size", "gauge", "当前本地缓存条目数", &Group::Stats::cache_size},
    };

    std::string out;

    for (const auto& def : kGroupMetrics) {
        AppendHelpType(out, def.name, def.type, def.help);
        for (const auto& kv : snapshots) {
            out += def.name;
            out += "{group=\"";
            out += PromLabelEscape(kv.first);
            out += "\"} ";
            out += std::to_string(kv.second.*(def.field));
            out += "\n";
        }
    }

    // 命中率单独算：它是派生量，不适合塞进上面的表（表里的字段都是直接可读的计数）。
    AppendHelpType(out, "dcache_group_hit_rate", "gauge",
                   "本地命中率 = local_hit / (local_hit + local_miss)");
    for (const auto& kv : snapshots) {
        const std::uint64_t hit   = kv.second.local_hit;
        const std::uint64_t denom = hit + kv.second.local_miss;
        const double rate =
            denom == 0 ? 0.0 : static_cast<double>(hit) / static_cast<double>(denom);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", rate);
        out += "dcache_group_hit_rate{group=\"";
        out += PromLabelEscape(kv.first);
        out += "\"} ";
        out += buf;
        out += "\n";
    }

    AppendHelpType(out, "dcache_hops_rejected_total", "counter",
                   "携带 X-DCache-Hops 但未通过可信校验的请求数（伪造内部头的信号）");
    out += "dcache_hops_rejected_total ";
    out += std::to_string(hops_rejected_.load(std::memory_order_relaxed));
    out += "\n";

    if (metrics_hook_) {
        metrics_hook_(out);
    }
    return out;
}

}  // namespace dcache