#include "Cache/MeshManager.h"
#include "Cache/ChunkNeighborCache.h"
#include "Render/ChunkMeshBuilder.h"
#include "World/ChunkView.h"
#include "World/World.h"

MeshManager::MeshManager(World& world) {
    meshProvider_ = std::make_unique<ChunkMeshProvider>(&world, &meshCache_);
}

MeshManager::~MeshManager() {
    shuttingDown_ = true;
    meshBuildGroup_.wait();
}

// FNV-1a over the block array.
uint64_t MeshManager::HashChunkData(const uint16_t* blocks, size_t count) {
    if (!blocks) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < count; ++i) {
        h ^= blocks[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

void MeshManager::OnBlockUpdate(BlockPos pos, uint16_t block_id, uint8_t meta,
                                 uint32_t mb_id, World& world) {
    world.OnBlockUpdate(pos, block_id, meta, mb_id);

    auto rebuildChunk = [this, &world](const ChunkCoord& c) {
        auto ch = world.GetChunk(c);
        if (!ch) return;
        if (!ch->blocks_data()) return;
        uint64_t h = HashChunkData(ch->blocks_data(), ch->blocks_size());
        if (meshCache_.CheckBuildHash(c, h))
            return;
        meshBuildGroup_.run([this, c, h, ch, &world] {
            if (shuttingDown_) return;
            ChunkNeighborCache cache;
            cache.Init(world, c, ch.get());
            auto meshData = ChunkMeshBuilder::Build(cache, ch);
            meshCache_.EnqueueCreateMesh(c, h, std::move(meshData));
        });
    };

    ChunkCoord coord{pos.x >> 5, pos.y >> 5, pos.z >> 5};
    rebuildChunk(coord);

    // rebuild neighbors — block on boundary affects adjacent chunk mesh
    int lx = pos.x & (CHUNK_SIZE - 1);
    int ly = pos.y & (CHUNK_SIZE - 1);
    int lz = pos.z & (CHUNK_SIZE - 1);
    if (lx == 0)       rebuildChunk({coord.x - 1, coord.y, coord.z});
    else if (lx == CHUNK_SIZE - 1) rebuildChunk({coord.x + 1, coord.y, coord.z});
    if (ly == 0)       rebuildChunk({coord.x, coord.y - 1, coord.z});
    else if (ly == CHUNK_SIZE - 1) rebuildChunk({coord.x, coord.y + 1, coord.z});
    if (lz == 0)       rebuildChunk({coord.x, coord.y, coord.z - 1});
    else if (lz == CHUNK_SIZE - 1) rebuildChunk({coord.x, coord.y, coord.z + 1});
}

void MeshManager::OnChunkData(ChunkCoord coord, std::shared_ptr<ChunkView> chunk,
                               World& world) {
    if (shuttingDown_) return;

    // Stale pending request: player moved away before server responded.
    if (!world.IsPending(coord))
        return;

    if (!chunk->blocks_data()) return;
    uint64_t hash = HashChunkData(chunk->blocks_data(), chunk->blocks_size());
    std::shared_ptr<const ChunkView> shared = world.OnChunkData(std::move(chunk), coord);
    if (meshCache_.CheckBuildHash(coord, hash))
        return;

    meshBuildGroup_.run([this, coord, hash, shared, &world] {
        if (shuttingDown_) return;
        // Chunk may have been evicted while mesh build was in flight.
        // Without this guard we create an orphan mesh entry that is never
        // cleaned up (the destroy was already processed before we enqueued).
        if (!world.HasChunk(coord)) return;
        ChunkNeighborCache cache;
        cache.Init(world, coord, shared.get());
        auto meshData = ChunkMeshBuilder::Build(cache, shared);
        meshCache_.EnqueueCreateMesh(coord, hash, std::move(meshData));
    });
}

void MeshManager::ProcessPendingOps() {
    meshCache_.ProcessPendingOps();
}

void MeshManager::HandleEviction(const ChunkCoord& coord) {
    meshCache_.EnqueueDestroyMesh(coord);
}

void MeshManager::DiscardHandles() {
    meshCache_.DiscardHandles();
}
