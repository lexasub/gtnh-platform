#pragma once

#include <cstdint>
#include <memory>
#include "../Common/Types.h"

class ChunkView;
class World;

// L1 cache: pre-resolves 6 neighboring chunk pointers for O(1) block lookup
// across chunk boundaries. Used by ChunkMeshBuilder to eliminate ~200k hash
// lookups per chunk build.
//
// Direction indices: [0]=+X, [1]=-X, [2]=+Y, [3]=-Y, [4]=+Z, [5]=-Z
class ChunkNeighborCache {
public:
    ChunkNeighborCache() = default;

    // Populate cache — fetches 6 neighbors via World::GetChunk.
    // centerChunk raw pointer must remain valid for the cache lifetime.
    void Init(const World& world, const ChunkCoord& centerCoord,
              const ChunkView* centerChunk);

    // Get block at (bx, by, bz) in center-chunk-local coords.
    // Values -1..32 wrap to adjacent chunk automatically.
    // Returns 0 if block is air or neighboring chunk not loaded.
    uint16_t GetBlock(int bx, int by, int bz) const;

    const ChunkView* CenterChunk() const { return centerChunk_; }

private:
    std::shared_ptr<const ChunkView> holders_[6];
    const ChunkView* nchunks_[6] = {};
    const ChunkView* centerChunk_ = nullptr;
};
