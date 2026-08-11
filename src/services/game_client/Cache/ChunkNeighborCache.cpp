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
    // Capture flat export pointers once (each blocks_data()/meta_data() takes
    // the chunk mutex internally, but only for the duration of the call).
    // SetBlock keeps them valid by updating in place, so the mesh builder can
    // read them lock-free afterwards.
    blocks_[0] = centerChunk ? centerChunk->blocks_data() : nullptr;
    meta_[0] = centerChunk ? centerChunk->meta_data() : nullptr;
    for (int d = 0; d < 6; ++d) {
        if (nchunks_[d]) {
            blocks_[d + 1] = nchunks_[d]->blocks_data();
            meta_[d + 1] = nchunks_[d]->meta_data();
        } else {
            blocks_[d + 1] = nullptr;
            meta_[d + 1] = nullptr;
        }
    }
}

uint16_t ChunkNeighborCache::GetBlock(int bx, int by, int bz) const {
    int inX = static_cast<unsigned>(bx) < CHUNK_SIZE;
    int inY = static_cast<unsigned>(by) < CHUNK_SIZE;
    int inZ = static_cast<unsigned>(bz) < CHUNK_SIZE;

    if (inX & inY & inZ) [[likely]] {
        return blocks_[0] ? blocks_[0][(by << 10) | (bz << 5) | bx] : 0;
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

    const uint16_t* b = blocks_[idx + 1];
    return b ? b[(ly << 10) | (lz << 5) | lx] : 0;
}

uint8_t ChunkNeighborCache::GetMeta(int bx, int by, int bz) const {
    int inX = static_cast<unsigned>(bx) < CHUNK_SIZE;
    int inY = static_cast<unsigned>(by) < CHUNK_SIZE;
    int inZ = static_cast<unsigned>(bz) < CHUNK_SIZE;

    if (inX & inY & inZ) [[likely]] {
        return meta_[0] ? meta_[0][(by << 10) | (bz << 5) | bx] : 0;
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

    const uint8_t* m = meta_[idx + 1];
    return m ? m[(ly << 10) | (lz << 5) | lx] : 0;
}
