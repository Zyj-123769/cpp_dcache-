#include "hash.hpp"

namespace dcache {

namespace {

inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

uint32_t murmur3_32(const void* data, size_t len, uint32_t seed) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const int nblocks    = static_cast<int>(len / 4);

    uint32_t h1 = seed;

    const uint32_t c1 = 0xcc9e2d51u;
    const uint32_t c2 = 0x1b873593u;

    // Body：按 4 字节块处理（小端手动组装，避免未对齐解引用与 strict-aliasing UB）。
    for (int i = 0; i < nblocks; ++i) {
        uint32_t k1 = read_le32(bytes + i * 4);

        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;

        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64u;
    }

    // Tail：处理不足 4 字节的末尾。
    const uint8_t* tail = bytes + nblocks * 4;
    uint32_t k1         = 0;
    switch (len & 3) {
    case 3:
        k1 ^= static_cast<uint32_t>(tail[2]) << 16;
        [[fallthrough]];
    case 2:
        k1 ^= static_cast<uint32_t>(tail[1]) << 8;
        [[fallthrough]];
    case 1:
        k1 ^= static_cast<uint32_t>(tail[0]);
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;
        h1 ^= k1;
    }

    // Finalization。
    h1 ^= static_cast<uint32_t>(len);
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6bu;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35u;
    h1 ^= h1 >> 16;

    return h1;
}

}  // namespace dcache