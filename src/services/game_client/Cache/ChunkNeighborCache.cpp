#include "ChunkNeighborCache.h"
#include "../World/ChunkView.h"
#include "../World/World.h"

void ChunkNeighborCache::Init(const World& world, const ChunkCoord& centerCoord,
                              const ChunkView* centerChunk) {
    centerChunk_ = centerChunk;
    static constexpr int dirs[6][3] = {
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
    };
    for (int d = 0; d < 6; ++d) {
        auto owner = world.GetChunk({centerCoord.x + dirs[d][0],
                                     centerCoord.y + dirs[d][1],
                                     centerCoord.z + dirs[d][2]});
        if (owner) {
            holders_[d] = std::move(owner);
            nchunks_[d] = holders_[d].get();
        } else {
            nchunks_[d] = nullptr;
        }
    }
}

uint16_t ChunkNeighborCache::GetBlock(int bx, int by, int bz) const {
    int inX = static_cast<unsigned>(bx) < CHUNK_SIZE;
    int inY = static_cast<unsigned>(by) < CHUNK_SIZE;
    int inZ = static_cast<unsigned>(bz) < CHUNK_SIZE;

    if (inX & inY & inZ) [[likely]] {
        return centerChunk_->GetBlock(bx, by, bz);
    }

    int oobX = inX ^ 1;
    int oobY = inY ^ 1;
    int oobZ = inZ ^ 1;

    int sideX = bx >> 31 & 1;
    int sideY = by >> 31 & 1;
    int sideZ = bz >> 31 & 1;

    int idx = oobX * sideX
            + oobY * (2 + sideY)
            + oobZ * (4 + sideZ);

    int lx = oobX * (sideX * (CHUNK_SIZE - 1)) + (oobX ^ 1) * bx;
    int ly = oobY * (sideY * (CHUNK_SIZE - 1)) + (oobY ^ 1) * by;
    int lz = oobZ * (sideZ * (CHUNK_SIZE - 1)) + (oobZ ^ 1) * bz;

    const ChunkView* chunk = idx < 6 ? nchunks_[idx] : centerChunk_;
    return chunk ? chunk->GetBlock(lx, ly, lz) : 0;
}
