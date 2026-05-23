#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Shared coordinate types extracted from game_client/Common/Types.h
//
// These are the canonical definitions for world/chunk coordinates used
// across all services. FlatBuffers 'struct Vec3i { x:int32; y:int32; z:int32; }'
// is the binary wire format; these C++ structs have the same layout.
// ---------------------------------------------------------------------------

// World block position (32-bit = ±2M blocks)
struct BlockPos {
    int32_t x = 0, y = 0, z = 0;
    bool operator==(const BlockPos& other) const { return x == other.x && y == other.y && z == other.z; }
    bool operator!=(const BlockPos& other) const { return !(*this == other); }
};

// Chunk coordinates (each chunk is 32x32x32 blocks)
struct ChunkCoord {
    int32_t x = 0, y = 0, z = 0;
    bool operator==(const ChunkCoord& other) const { return x == other.x && y == other.y && z == other.z; }
    bool operator!=(const ChunkCoord& other) const { return !(*this == other); }
};

// Chunk key range: ±2^20 = ±1,048,576 chunks per dimension
constexpr int64_t CHUNK_KEY_BIAS = 1LL << 20;

// 64-bit key for chunk coordinates (used in maps)
inline uint64_t MakeChunkKey(ChunkCoord c) {
    uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(c.x) + CHUNK_KEY_BIAS);
    uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(c.y) + CHUNK_KEY_BIAS);
    uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(c.z) + CHUNK_KEY_BIAS);
    return (x << 42) | (y << 21) | z;
}

inline uint64_t MakeBlockPosKey(int32_t x, int32_t y, int32_t z) {
    uint64_t ux = static_cast<uint64_t>(static_cast<int64_t>(x) + CHUNK_KEY_BIAS);
    uint64_t uy = static_cast<uint64_t>(static_cast<int64_t>(y) + CHUNK_KEY_BIAS);
    uint64_t uz = static_cast<uint64_t>(static_cast<int64_t>(z) + CHUNK_KEY_BIAS);
    return (ux << 42) | (uy << 21) | uz;
}

inline ChunkCoord ChunkKeyToCoord(uint64_t key) {
    ChunkCoord c;
    c.x = static_cast<int32_t>(static_cast<int64_t>((key >> 42) & 0x1FFFFF) - CHUNK_KEY_BIAS);
    c.y = static_cast<int32_t>(static_cast<int64_t>((key >> 21) & 0x1FFFFF) - CHUNK_KEY_BIAS);
    c.z = static_cast<int32_t>(static_cast<int64_t>(key & 0x1FFFFF) - CHUNK_KEY_BIAS);
    return c;
}

// Constants
constexpr int CHUNK_SIZE = 32;
constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE; // 32768
