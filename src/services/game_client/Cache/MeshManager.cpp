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

    ChunkCoord coord{pos.x >> 5, pos.y >> 5, pos.z >> 5};
    auto chunk = world.GetChunk(coord);
    if (!chunk) return;

    uint64_t hash = HashChunkData(chunk->blocks_data(), chunk->blocks_size());
    if (meshCache_.CheckBuildHash(coord, hash))
        return;

    meshBuildGroup_.run([this, coord, hash, chunk, &world] {
        if (shuttingDown_) return;
        ChunkNeighborCache cache;
        cache.Init(world, coord, chunk.get());
        auto meshData = ChunkMeshBuilder::Build(cache, chunk);
        meshCache_.EnqueueCreateMesh(coord, hash, std::move(meshData));
    });
}

void MeshManager::OnChunkData(ChunkCoord coord, std::shared_ptr<ChunkView> chunk,
                               World& world) {
    if (shuttingDown_) return;

    // Stale pending request: player moved away before server responded.
    if (!world.IsPending(coord))
        return;

    uint64_t hash = HashChunkData(chunk->blocks_data(), chunk->blocks_size());
    std::shared_ptr<const ChunkView> shared = world.OnChunkData(std::move(chunk), coord);
    if (meshCache_.CheckBuildHash(coord, hash))
        return;

    meshBuildGroup_.run([this, coord, hash, shared, &world] {
        if (shuttingDown_) return;
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
