#include "consistent_hash.hpp"

#include "hash.hpp"

namespace dcache {

namespace {

std::string vnode_key(const std::string& node, int i) {
    return node + "#" + std::to_string(i);
}

}  // namespace

ConsistentHash::ConsistentHash(int virtual_nodes) : virtual_nodes_(virtual_nodes) {
    if (virtual_nodes_ < 1) {
        virtual_nodes_ = 1;
    }
}

std::uint32_t ConsistentHash::hash(const std::string& s) const {
    return murmur3_32(s);
}

void ConsistentHash::AddNode(const std::string& node) {
    if (node.empty()) {
        return;
    }
    if (vnodes_.find(node) != vnodes_.end()) {
        return;  // 已存在，幂等
    }

    std::vector<std::uint32_t> hashes;
    hashes.reserve(static_cast<std::size_t>(virtual_nodes_));
    for (int i = 0; i < virtual_nodes_; ++i) {
        std::uint32_t h = hash(vnode_key(node, i));
        // 哈希冲突时线性探测到下一个空位。
        // 若直接 ring_[h] = node 覆盖，冲突位置的另一个节点虚拟节点会消失，
        // 而 RemoveNode 又会按 hash 误删这个位置，导致部分 key 永久路由丢失。
        while (ring_.find(h) != ring_.end()) {
            ++h;
        }
        hashes.push_back(h);
        ring_[h] = node;
    }
    vnodes_[node] = std::move(hashes);
}

void ConsistentHash::RemoveNode(const std::string& node) {
    auto it = vnodes_.find(node);
    if (it == vnodes_.end()) {
        return;
    }
    for (std::uint32_t h : it->second) {
        auto rit = ring_.find(h);
        // 校验归属再删：该位置可能已被其他节点占用，误删会让对方的虚拟节点消失。
        if (rit != ring_.end() && rit->second == node) {
            ring_.erase(rit);
        }
    }
    vnodes_.erase(it);
}

std::string ConsistentHash::GetNode(const std::string& key) const {
    if (ring_.empty()) {
        return "";
    }

    std::uint32_t h = hash(key);
    auto it         = ring_.lower_bound(h);  // 顺时针：第一个 >= h 的虚拟节点
    if (it == ring_.end()) {
        it = ring_.begin();  // 越过环尾，回绕到 0
    }
    return it->second;
}

bool ConsistentHash::Contains(const std::string& node) const {
    return vnodes_.find(node) != vnodes_.end();
}

std::size_t ConsistentHash::NodeCount() const {
    return vnodes_.size();
}
std::size_t ConsistentHash::RingSize() const {
    return ring_.size();
}
int ConsistentHash::VirtualNodes() const {
    return virtual_nodes_;
}

}  // namespace dcache