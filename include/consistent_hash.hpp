#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dcache {

// 一致性哈希环：把 key 映射到负责节点。
//
// 每个物理节点在环上映射 virtual_nodes_ 个虚拟节点（以 "node#i" 作哈希键），
// 解决物理节点哈希分布不均导致的负载倾斜，同时实现平滑扩缩容——
// 新增节点只接管环上相邻一段（约 1/N 的 key），迁移代价最小。
class ConsistentHash {
public:
    explicit ConsistentHash(int virtual_nodes = 150);

    void AddNode(const std::string& node);
    void RemoveNode(const std::string& node);

    // 返回负责该 key 的节点地址；空环返回 ""。
    std::string GetNode(const std::string& key) const;

    bool                    Contains(const std::string& node) const;
    std::size_t             NodeCount() const;  // 物理节点数
    std::size_t             RingSize() const;   // 虚拟节点数
    int                     VirtualNodes() const;

private:
    std::uint32_t hash(const std::string& s) const;

    int                                      virtual_nodes_;
    std::map<std::uint32_t, std::string>     ring_;   // 虚拟节点哈希 -> 物理节点
    std::unordered_map<std::string, std::vector<std::uint32_t>> vnodes_; // 物理节点 -> 其虚拟节点哈希列表
};

} // namespace dcache