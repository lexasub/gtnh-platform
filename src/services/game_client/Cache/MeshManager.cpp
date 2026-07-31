#include "Cache/MeshManager.h"
#include "Cache/ChunkNeighborCache.h"
#include "Render/ChunkMeshBuilder.h"
#include "World/ChunkView.h"
#include "World/World.h"
#include <chrono>
#include <spdlog/spdlog.h>

MeshManager::MeshManager(World& world) {
    meshProvider_ = std::make_unique<ChunkMeshProvider>(&world, &meshCache_);
}

MeshManager::~MeshManager() {
    shuttingDown_ = true;
    loadGroup_.wait();
    updateGroup_.wait();
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

// Atomically check if a rebuild is already in-flight for this chunk.
// Returns true if we claimed it (caller should proceed), false if already queued.
static bool tryClaimRebuild(
    std::mutex &mtx,
    std::unordered_map<uint64_t, bool> &pending,
    uint64_t key)
{
    std::lock_guard lock(mtx);
    if (pending[key])
        return false;
    pending[key] = true;
    return true;
}

static void releaseRebuild(
    std::mutex &mtx,
    std::unordered_map<uint64_t, bool> &pending,
    uint64_t key)
{
    std::lock_guard lock(mtx);
    pending.erase(key);
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

        uint64_t key = MakeChunkKey(c);
        if (!tryClaimRebuild(pendingRebuildMtx_, pendingRebuilds_, key))
            return; // already queued — skip duplicate

        auto t_enq = std::chrono::steady_clock::now();
        updateGroup_.run([this, c, ch, &world, t_enq, key] {
            if (shuttingDown_) return;
            auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_enq).count();
            if (wait_us > 1000) {
                spdlog::trace("MeshManager: chunk ({},{},{}) waited {} us in TBB queue",
                              c.x, c.y, c.z, wait_us);
            }
            // Chunk may have been evicted while we were waiting in queue
            if (!world.HasChunk(c)) {
                releaseRebuild(pendingRebuildMtx_, pendingRebuilds_, key);
                return;
            }
            // Recompute hash from current data (may have changed since queue time)
            if (!ch->blocks_data()) {
                releaseRebuild(pendingRebuildMtx_, pendingRebuilds_, key);
                return;
            }
            uint64_t h_actual = HashChunkData(ch->blocks_data(), ch->blocks_size());
            ChunkNeighborCache cache;
            cache.Init(world, c, ch.get());
            auto meshData = ChunkMeshBuilder::Build(cache, ch);
            meshCache_.EnqueueCreateMesh(c, h_actual, std::move(meshData));
            releaseRebuild(pendingRebuildMtx_, pendingRebuilds_, key);
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

    uint64_t key = MakeChunkKey(coord);
    if (!tryClaimRebuild(pendingRebuildMtx_, pendingRebuilds_, key))
        return; // already queued — skip duplicate

    auto t_enq = std::chrono::steady_clock::now();
    loadGroup_.run([this, coord, shared, &world, t_enq, key] {
        if (shuttingDown_) return;
        auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_enq).count();
        if (wait_us > 1000) {
            spdlog::trace("MeshManager: chunk data ({},{},{}) waited {} us in TBB queue",
                          coord.x, coord.y, coord.z, wait_us);
        }
        // Chunk may have been evicted while mesh build was in flight.
        // Without this guard we create an orphan mesh entry that is never
        // cleaned up (the destroy was already processed before we enqueued).
        if (!world.HasChunk(coord)) {
            releaseRebuild(pendingRebuildMtx_, pendingRebuilds_, key);
            return;
        }
        if (!shared->blocks_data()) {
            releaseRebuild(pendingRebuildMtx_, pendingRebuilds_, key);
            return;
        }
        uint64_t h_actual = HashChunkData(shared->blocks_data(), shared->blocks_size());
        ChunkNeighborCache cache;
        cache.Init(world, coord, shared.get());
        auto meshData = ChunkMeshBuilder::Build(cache, shared);
        meshCache_.EnqueueCreateMesh(coord, h_actual, std::move(meshData));
        releaseRebuild(pendingRebuildMtx_, pendingRebuilds_, key);
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
