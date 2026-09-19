#pragma once
#include <cstddef>
#include <cstdint>

namespace mesh {

// FNV-1a (64-bit) over block ids then per-block meta. Meta MUST be mixed in so
// a meta-only change (e.g. a pipe connection toggle) yields a different hash and
// forces a mesh rebuild — otherwise the client skips rebuilding on a toggle.
inline uint64_t HashBlockData(const uint16_t* blocks, size_t bcount,
                              const uint8_t* meta, size_t mcount) {
    uint64_t h = 0xcbf29ce484222325ull;
    if (blocks) {
        for (size_t i = 0; i < bcount; ++i) {
            h ^= blocks[i];
            h *= 0x100000001b3ull;
        }
    }
    if (meta) {
        for (size_t i = 0; i < mcount; ++i) {
            h ^= meta[i];
            h *= 0x100000001b3ull;
        }
    }
    return h;
}
}  // namespace mesh
