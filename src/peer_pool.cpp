#include "peer_pool.hpp"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

#include "auth.hpp"
#include "http_client.hpp"
#include "json.hpp"
#include "url.hpp"

namespace dcache {

namespace {

// 单次转发到对端的超时。超过即判定为「没问到」（Error），不参与负缓存。
constexpr int kPeerTimeoutMs = 2000;

// 解析 "host:port"。
bool split_addr(const std::string& addr, std::string& host, std::uint16_t& port) {
    const std::size_t colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= addr.size()) {
        return false;
    }
    host  = addr.substr(0, colon);
    int p = std::atoi(addr.substr(colon + 1).c_str());
    if (p <= 0 || p > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(p);
    return true;
}

}  // namespace

HttpPeerPool::HttpPeerPool(std::string self_addr, int virtual_nodes)
    : self_(std::move(self_addr)), ring_(std::make_shared<ConsistentHash>(virtual_nodes)) {}

void HttpPeerPool::SetPeers(const std::vector<std::string>& peers) {
    // 用相同虚拟节点数重建环，确保新旧一致。
    auto rebuilt = std::make_shared<ConsistentHash>(VirtualNodes());
    std::unordered_set<std::string> hosts;
    for (const auto& p : peers) {
        rebuilt->AddNode(p);

        std::string host;
        std::uint16_t port = 0;
        if (split_addr(p, host, port)) {
            hosts.insert(host);
        }
    }

    // 先发布 host 集合再换环：IsTrustedHost 与 PickPeer 是两条独立路径，
    // 若顺序反过来，会有一小段时间「环里已有新节点、但它的 host 还不被信任」。
    {
        std::lock_guard<std::mutex> lk(peers_mtx_);
        peer_hosts_.swap(hosts);
    }

    // 原子换指针：已经拿到旧快照的读线程继续安全使用旧环，直到最后一个引用释放。
    std::atomic_store(&ring_, std::shared_ptr<const ConsistentHash>(std::move(rebuilt)));
}

int HttpPeerPool::VirtualNodes() const {
    auto ring = Snapshot();
    return ring ? ring->VirtualNodes() : 0;
}

bool HttpPeerPool::IsTrustedHost(const std::string& host) const {
    if (host.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(peers_mtx_);
    return peer_hosts_.find(host) != peer_hosts_.end();
}

void HttpPeerPool::SetAuthToken(std::string token) {
    std::lock_guard<std::mutex> lk(peers_mtx_);
    auth_token_ = std::move(token);
}

std::string HttpPeerPool::AuthToken() const {
    std::lock_guard<std::mutex> lk(peers_mtx_);
    return auth_token_;
}

bool HttpPeerPool::IsTrustedRequest(const std::string& remote_ip,
                                    const std::string& token_header) const {
    std::lock_guard<std::mutex> lk(peers_mtx_);

    if (!auth_token_.empty()) {
        // 配了 token 就只认 token。此时 IP 判定不再参与——两者取或等于取最弱的那个。
        return ConstantTimeEquals(auth_token_, token_header);
    }
    return !remote_ip.empty() && peer_hosts_.find(remote_ip) != peer_hosts_.end();
}

std::string HttpPeerPool::PickPeer(const std::string& key) const {
    auto ring = Snapshot();
    if (!ring) {
        return "";
    }
    std::string node = ring->GetNode(key);
    if (node.empty() || node == self_) {
        return "";
    }
    return node;
}

GetResult HttpPeerPool::GetFromPeer(const std::string& peer, const std::string& group,
                                    const std::string& key, int hops) const {
    std::string host;
    std::uint16_t port = 0;
    if (!split_addr(peer, host, port)) {
        return GetResult::Error("bad peer address: " + peer);
    }

    // 转义 group/key 中的特殊字符，避免破坏请求路径。
    const std::string path = "/api/" + UrlEncodeComponent(group) + "/" + UrlEncodeComponent(key);

    std::map<std::string, std::string> headers;
    if (hops > 0) {
        headers["X-DCache-Hops"] = std::to_string(hops);
    }

    // 内部共享 token。即使 hops==0 也一并发送：接收方判定「可信」的依据就是这个头，
    // 与是否需要跳数无关；如果只在 hops>0 时发送，接收方就无法区分「内部请求」与「外部请求」。
    //
    // ⚠ 局限：节点间是明文 HTTP，token 在网络上是可见的。这里解决的是「外部客户端
    // 无法伪造内部语义」的应用层信任问题，不能替代传输层加密。跨不可信网络部署时，
    // 应在节点间加 TLS 或走内网隔离。
    const std::string token = AuthToken();
    if (!token.empty()) {
        headers["X-DCache-Token"] = token;
    }

    int http_status = 0;
    std::string body;
    if (!HttpClient::Get(host, port, path, body, kPeerTimeoutMs, headers, &http_status)) {
        // 这里必须返回 Error 而不是「未命中」：超时、连接被拒、对端 5xx 都只说明
        // 「这次没问到」，与「数据不存在」是两件事。旧实现一律当 not-found，
        // 于是对端抖 2 秒、用户连续 30 秒看到数据不存在。
        if (http_status == 0) {
            return GetResult::Error("peer " + peer + " unreachable or timed out");
        }
        return GetResult::Error("peer " + peer + " returned HTTP " + std::to_string(http_status));
    }

    std::string value;
    bool found = false;
    if (!DecodeResult(body, value, found)) {
        return GetResult::Error("peer " + peer + " sent a malformed response");
    }
    return found ? GetResult::Ok(std::move(value)) : GetResult::NotFound();
}

}  // namespace dcache