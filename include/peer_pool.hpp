#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "common.hpp"
#include "consistent_hash.hpp"

namespace dcache {

// 节点选择抽象（对应 groupcache 的 PeerPicker / Peer 接口）。
class PeerPicker {
public:
    virtual ~PeerPicker() = default;

    // 返回负责该 key 的节点地址；空串表示本机负责（或未配置远端）。
    virtual std::string PickPeer(const std::string& key) const = 0;

    // 从指定节点地址（host:port）读取 group 下的 key。hops 为已转发跳数，随请求头传递用于防环。
    virtual GetResult GetFromPeer(const std::string& peer, const std::string& group,
                                  const std::string& key, int hops) const = 0;
};

// 基于一致性哈希 + HTTP 的节点选择 / 转发实现。
class HttpPeerPool : public PeerPicker {
public:
    explicit HttpPeerPool(std::string self_addr, int virtual_nodes = 150);

    // 重建哈希环为 peers 列表（通常含自身）。
    void SetPeers(const std::vector<std::string>& peers);

    // host 是否为已配置的集群节点（host 为 IPv4 字面量）。
    //
    // 用途：决定是否信任 X-DCache-Hops 这类「内部语义头」。外部客户端可以随意伪造该头，
    // 若不加来源校验，就能强制任意节点绕过一致性哈希直接回源，把流量全压到数据源。
    //
    // 注意：IP 判定只能挡住「集群外的机器」，挡不住同机/同网段的其他进程（NAT 后共享 IP
    // 更是全部互通）。真正可靠的判据是 SetAuthToken 配置的共享 token。
    bool IsTrustedHost(const std::string& host) const;

    // 内部共享 token。为空表示不校验（仅限单机开发）。
    void SetAuthToken(std::string token);
    std::string AuthToken() const;

    // 是否为可信的内部请求（决定能否采信 X-DCache-Hops）。
    //
    //   配置了 token：只认 token（常量时间比较），IP 不参与判定；
    //   未配置 token：退回 IP 判定，且调用方应打印一次告警。
    //
    // 之所以「配置了 token 就不再参考 IP」，是因为两者取或等于取最弱的那一个：
    // 只要还有一个可绕过的通道，攻击者就会走那条路。
    bool IsTrustedRequest(const std::string& remote_ip, const std::string& token_header) const;

    const std::string& Self() const { return self_; }
    int VirtualNodes() const;

    std::string PickPeer(const std::string& key) const override;
    GetResult GetFromPeer(const std::string& peer, const std::string& group, const std::string& key,
                          int hops) const override;

private:
    // 取当前哈希环快照。
    std::shared_ptr<const ConsistentHash> Snapshot() const { return std::atomic_load(&ring_); }

    std::string self_;

    // 哈希环按「不可变快照 + 原子换指针」的方式更新：
    // SetPeers 构造出全新的环再整体替换，读侧拿到的是某个完整版本，
    // 不会读到「正在被 AddNode 改写」的 std::map（旧实现在扩容时会读到半更新状态，甚至段错误）。
    // 用 shared_ptr 的原子自由函数而不是互斥量，是为了让 PickPeer 这条每请求必经的热路径不加锁。
    std::shared_ptr<const ConsistentHash> ring_;

    // peers 的 host 集合，独立于 ring_ 的锁（只保护自己的小集合，不与环重建产生锁序问题）。
    // auth_token_ 复用同一把锁：两者都只在「内部请求判定」这条路径上读，
    // 写只在启动时发生一次，争用可以忽略。
    mutable std::mutex peers_mtx_;
    std::unordered_set<std::string> peer_hosts_;
    std::string auth_token_;
};

}  // namespace dcache