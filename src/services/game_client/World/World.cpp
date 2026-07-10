#include "World.h"
#include "../Network/NetClient.h"
#include "ChunkView.h"
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ranges>
#include <tbb/parallel_for.h>
#include <tbb/concurrent_vector.h>

//#define FAST_SEND

World::World() = default;
World::~World() = default;

struct ScoredChunk {
    ChunkCoord coord;
    float priority;
};

void World::GetNewChunksFromServer(const Frustum& frustum, glm::vec3 cameraPos,
                                    glm::vec3 forward, glm::vec3 velocity,
                                    NetClient& netClient) {
    int centerX = static_cast<int>(std::floor(cameraPos.x / CHUNK_SIZE));
    int centerY = static_cast<int>(std::floor(cameraPos.y / CHUNK_SIZE));
    int centerZ = static_cast<int>(std::floor(cameraPos.z / CHUNK_SIZE));

    int R = VIEW_RADIUS;
    int side = 2 * R + 1;
    int total = side * side * side;

    std::vector<ScoredChunk> candidates;

    for (int idx = 0; idx < total; ++idx) {
        ChunkCoord coord{
            (centerX + (idx / (side * side) - R)),
            (centerY + (idx / side % side - R)),
            (centerZ + (idx % side - R))
        };

        uint64_t key = MakeChunkKey(coord);

        if (storage_.HasChunk(coord))
            continue;

        if (pendingRequests_.contains(key))
            continue;

        // Frustum cull: chunk bounds
        glm::vec3 min(coord.x * CHUNK_SIZE, coord.y * CHUNK_SIZE, coord.z * CHUNK_SIZE);
        glm::vec3 max(min.x + CHUNK_SIZE, min.y + CHUNK_SIZE, min.z + CHUNK_SIZE);
        if (!frustum.IntersectsAABB(min, max))
            continue;

        // Priority scoring: closer + in look direction + in movement direction = higher
        candidates.push_back({coord, calcCandidateScore(min, cameraPos, forward, velocity)});//TODO - may be priority queue (or partial order priority queue)
    };

    // Sort by priority descending — chunks where player looks/moves go first
    std::ranges::sort(candidates,
                      [](const ScoredChunk& a, const ScoredChunk& b) {
                          return a.priority > b.priority;
                      });

    // Submit in priority order
    for (const auto& req : candidates) {
        uint64_t key = MakeChunkKey(req.coord);
        #ifdef FAST_SEND
                netClient.RequestChunk(req.coord);
                pendingRequests_.insert(key);
        #else
                if (pendingRequests_.insert(key).second) {
                    netClient.RequestChunk(req.coord);
                }
        #endif
    }

    EvictFarChunks(cameraPos);
}

bool World::HasChunk(const ChunkCoord &coord) const {
    return storage_.HasChunk(coord);
}

float World::calcCandidateScore(glm::vec3 min, glm::vec3 cameraPos, glm::vec3 forward, glm::vec3 velocity) {
    glm::vec3 chunkCenter(min.x + CHUNK_SIZE * 0.5f, min.y + CHUNK_SIZE * 0.5f, min.z + CHUNK_SIZE * 0.5f);
    glm::vec3 toChunk = chunkCenter - cameraPos;
    float dist = glm::length(toChunk) + 0.1f;

    float score = 1.0f / dist;

    glm::vec3 dir = toChunk / dist;
    float lookDot = glm::dot(dir, forward);
    if (lookDot > 0.0f) {
        score += lookDot * lookDot * 2.0f / dist;
    }

    float speed = glm::length(velocity);
    if (speed > 0.1f) {
        glm::vec3 velDir = velocity / speed;
        float velDot = glm::dot(dir, velDir);
        if (velDot > 0.0f) {
            return score + velDot * 1.0f / dist;
        }
    }
    return score;
}

void World::EvictFarChunks(glm::vec3 cameraPos) {
    if (storage_.Size() <= MAX_CHUNKS)
        return;

    // Collect all chunks with their distances
    struct DistEntry { float distSq; ChunkCoord coord; };
    std::vector<DistEntry> entries;
    entries.reserve(storage_.Size());

    storage_.ForEachCoord([&](ChunkCoord coord, const std::shared_ptr<ChunkView>&) {
        glm::vec3 center(coord.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f,
                          coord.y * CHUNK_SIZE + CHUNK_SIZE * 0.5f,
                          coord.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
        glm::vec3 diff = center - cameraPos;
        entries.push_back({glm::dot(diff, diff), coord});
    });

    std::ranges::sort(entries,
                      [](const DistEntry& a, const DistEntry& b) { return a.distSq > b.distSq; });

    size_t excess = storage_.Size() > MAX_CHUNKS ? storage_.Size() - MAX_CHUNKS : 0;
    size_t evictCount = std::min(excess, entries.size());

    {
        std::lock_guard lock(pendingEvictedMtx_);
        pendingEvicted_.clear();
        pendingEvicted_.reserve(evictCount);
        for (size_t i = 0; i < evictCount; ++i) {
            pendingEvicted_.push_back(entries[i].coord);
        }
    }

    for (size_t i = 0; i < evictCount; ++i) {
        storage_.RemoveChunk(entries[i].coord);

        uint64_t key = MakeChunkKey(entries[i].coord);
        pendingRequests_.unsafe_erase(key);
    }
}

void World::EvictChunk(const ChunkCoord& coord) {
    uint64_t key = MakeChunkKey(coord);
    pendingChanges_.erase(key);
    storage_.RemoveChunk(coord);
    pendingRequests_.unsafe_erase(key);
    std::lock_guard lock(pendingEvictedMtx_);
    pendingEvicted_.push_back(coord);
}

bool World::TryRequestChunk(const ChunkCoord& coord, NetClient& netClient) {
    uint64_t key = MakeChunkKey(coord);
    if (pendingRequests_.insert(key).second) {
        netClient.RequestChunk(coord);
        return true;
    }
    return false;
}

std::vector<ChunkCoord> World::TakeEvictedChunks() {
    std::lock_guard lock(pendingEvictedMtx_);
    return std::move(pendingEvicted_);
}

uint16_t World::GetBlockAt(BlockPos pos) const {
    // Arithmetic right shift = floor division: -1 >> 5 = -1 (vs -1/32 = 0).
    ChunkCoord c{pos.x >> 5, pos.y >> 5, pos.z >> 5};
    auto chunk = storage_.GetChunk(c);
    if (!chunk)
        return 0;
    int lx = pos.x & (CHUNK_SIZE - 1);
    int ly = pos.y & (CHUNK_SIZE - 1);
    int lz = pos.z & (CHUNK_SIZE - 1);
    return chunk->GetBlock(lx, ly, lz);
}

std::shared_ptr<const ChunkView> World::GetChunk(const ChunkCoord &c) const {
    return storage_.GetChunk(c);
}

bool World::IsPending(const ChunkCoord& coord) const {
    return pendingRequests_.contains(MakeChunkKey(coord));
}

std::shared_ptr<const ChunkView> World::OnChunkData(std::shared_ptr<ChunkView> chunk, const ChunkCoord &coord) {
    auto chunkPtr = storage_.StoreAndGetChunk(coord, std::move(chunk));

    uint64_t key = MakeChunkKey(coord);

    // Rebase pending changes over the fresh snapshot.
    // Server might return stale chunk data (ChunkStore race: readTransaction vs
    // CAS), so we re-apply any block updates that were committed locally.
    auto it = pendingChanges_.find(key);
    if (it != pendingChanges_.end()) {
        for (const auto& [pk, pb] : it->second) {
            int64_t bx = static_cast<int64_t>((pk >> 42) & 0x1FFFFF) - CHUNK_KEY_BIAS;
            int64_t by = static_cast<int64_t>((pk >> 21) & 0x1FFFFF) - CHUNK_KEY_BIAS;
            int64_t bz = static_cast<int64_t>(pk & 0x1FFFFF) - CHUNK_KEY_BIAS;
            int lx = static_cast<int>(bx & (CHUNK_SIZE - 1));
            int ly = static_cast<int>(by & (CHUNK_SIZE - 1));
            int lz = static_cast<int>(bz & (CHUNK_SIZE - 1));
            chunkPtr->SetBlock(lx, ly, lz, pb.block_id, pb.meta, pb.mb_id);
        }
    }

    pendingRequests_.unsafe_erase(key);//TODO - warn - возможно map прийдется использовать) или возвращаться к stl коллекции если + lock - если будут проблемы
    return chunkPtr;
}

void World::OnBlockUpdate(BlockPos pos, uint16_t block_id, uint8_t meta, uint32_t mb_id) {
    ChunkCoord cc{pos.x >> 5, pos.y >> 5, pos.z >> 5};
    auto chunk = storage_.GetChunk(cc);
    if (!chunk) {
        // Chunk not loaded — server update will arrive when it is.
        // TODO(diff-protocol): cache pending updates for unloaded chunks
        return;
    }
    int lx = pos.x & (CHUNK_SIZE - 1);
    int ly = pos.y & (CHUNK_SIZE - 1);
    int lz = pos.z & (CHUNK_SIZE - 1);
    chunk->SetBlock(lx, ly, lz, block_id, meta, mb_id);

    // Save pending change so OnChunkData can re-apply it over stale snapshots.
    uint64_t ck = MakeChunkKey(cc);
    uint64_t pk = MakeBlockPosKey(pos.x, pos.y, pos.z);
    pendingChanges_[ck][pk] = {block_id, meta, mb_id};
}

bool World::IsBlockActionPending(BlockPos pos) const {
    BlockActionMap::const_accessor acc;
    return pendingBlockActions_.find(acc, MakeBlockPosKey(pos.x, pos.y, pos.z));
}

void World::MarkBlockActionSent(BlockPos pos) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    pendingBlockActions_.insert({MakeBlockPosKey(pos.x, pos.y, pos.z), now});
}

void World::ClearBlockActionPending(BlockPos pos) {
    pendingBlockActions_.erase(MakeBlockPosKey(pos.x, pos.y, pos.z));
}

static uint64_t BlockPosKeyToChunkKey(uint64_t block_key);
void World::ClearExpiredBlockActions(std::chrono::steady_clock::duration timeout) {
    auto deadline = (std::chrono::steady_clock::now() - timeout).time_since_epoch().count();
    std::vector<uint64_t> to_erase;
    for (auto& entry : pendingBlockActions_) {
        if (entry.second < deadline) {
            // Don't expire actions for chunks that are still being loaded.
            // The action is valid — we're just waiting for chunk data + CAS ack.
            auto chunkKey = BlockPosKeyToChunkKey(entry.first);
            if (pendingRequests_.contains(chunkKey)) {
                continue;
            }
            to_erase.push_back(entry.first);
        }
    }
    if (to_erase.empty()) return;
    for (auto key : to_erase) {
        pendingBlockActions_.erase(key);
    }
    spdlog::info("Cleared {} expired pending block actions", to_erase.size());
}

// Convert a block position key (as stored in pendingBlockActions_) to the
// corresponding chunk key (for lookup in pendingRequests_).
static uint64_t BlockPosKeyToChunkKey(uint64_t block_key) {
    // Extract biased block coordinates
    int64_t bx = static_cast<int64_t>((block_key >> 42) & 0x1FFFFF) - CHUNK_KEY_BIAS;
    int64_t by = static_cast<int64_t>((block_key >> 21) & 0x1FFFFF) - CHUNK_KEY_BIAS;
    int64_t bz = static_cast<int64_t>(block_key & 0x1FFFFF) - CHUNK_KEY_BIAS;

    // Convert to chunk coordinates
    int64_t cx = bx >> 5;
    int64_t cy = by >> 5;
    int64_t cz = bz >> 5;

    // Re-encode as chunk key
    uint64_t ucx = static_cast<uint64_t>(cx + CHUNK_KEY_BIAS);
    uint64_t ucy = static_cast<uint64_t>(cy + CHUNK_KEY_BIAS);
    uint64_t ucz = static_cast<uint64_t>(cz + CHUNK_KEY_BIAS);
    return (ucx << 42) | (ucy << 21) | ucz;
}

// pendingChanges_ are now populated in OnBlockUpdate and re-applied in
// OnChunkData (protection against stale chunk snapshots). Cleaned up on
// EvictChunk. No need to erase on BlockAck — repeated applies are idempotent
// and the change must survive until the chunk is evicted.