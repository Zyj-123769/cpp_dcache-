#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dcache {

// MurmurHash3 (32-bit) 精简实现，用作一致性哈希环的散列函数。
//  - 分布均匀，缓解虚拟节点负载倾斜；
//  - 同一输入跨进程结果稳定，保证所有节点对 key→节点 路由一致。
uint32_t murmur3_32(const void* data, size_t len, uint32_t seed = 0);

inline uint32_t murmur3_32(const std::string& s, uint32_t seed = 0) {
    return murmur3_32(s.data(), s.size(), seed);
}

}  // namespace dcache