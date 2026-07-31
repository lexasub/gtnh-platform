#include "ChunkLoadManager.h"
#include "../World/World.h"
#include "../World/ChunkView.h"
#include "../Network/NetClient.h"
#include "core_generated.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ranges>
#include <spdlog/spdlog.h>

ChunkLoadManager::ChunkLoadManager(World& world, NetClient& netClient)
    : world_(world), netClient_(netClient) {
}

void ChunkLoadManager::SetUpdateRate(float updatesPerSecond) {
    updateInterval_ = 1.0f / updatesPerSecond;
}

void ChunkLoadManager::Update(const Frustum& frustum, glm::vec3 cameraPos,
                               glm::vec3 forward, glm::vec3 velocity, float dt) {
    // Process eviction queue every frame (not rate-limited) so TTL decays smoothly
    ProcessEvictionQueue(dt);

    // Rate limit: don't re-scan every frame (30 Hz default)
    timeSinceUpdate_ += dt;
    if (timeSinceUpdate_ < updateInterval_)
        return;
    timeSinceUpdate_ = 0.0f;

    RunLoadPass(frustum, cameraPos, forward, velocity);
}

void ChunkLoadManager::RunLoadPass(const Frustum& frustum, glm::vec3 cameraPos,
                                    glm::vec3 forward, glm::vec3 velocity) {
    int centerX = static_cast<int>(std::floor(cameraPos.x / CHUNK_SIZE));
    int centerY = static_cast<int>(std::floor(cameraPos.y / CHUNK_SIZE));
    int centerZ = static_cast<int>(std::floor(cameraPos.z / CHUNK_SIZE));

    int R = World::VIEW_RADIUS;
    // Limit vertical range — chunks far above/below are just air or stone
    // and don't need generating. ±3 chunks covers terrain height variance.
    static constexpr int VERTICAL_RADIUS = 3;

    std::vector<ScoredChunk> candidates;

    int xzSide = 2 * R + 1;
    int ySide = 2 * VERTICAL_RADIUS + 1;
    int xzSlice = xzSide * ySide;
    int total = xzSide * xzSide * ySide;
    for (int idx = 0; idx < total; ++idx) {
        ChunkCoord coord{
            centerX + (idx / xzSlice) - R,
            centerY + (idx % xzSlice) % ySide - VERTICAL_RADIUS,
            centerZ + (idx % xzSlice) / ySide - R
        };

        if (world_.HasChunk(coord)) {
            // Chunk is loaded AND in load radius — cancel any pending eviction.
            // erase() is a no-op if not in the map, so this is safe for all coords.
            uint64_t key = MakeChunkKey(coord);
            evictionQueue_.erase(key);
            continue;
        }

        if (world_.IsPending(coord))
            continue;

        glm::vec3 min(coord.x * CHUNK_SIZE, coord.y * CHUNK_SIZE, coord.z * CHUNK_SIZE);
        glm::vec3 max(min.x + CHUNK_SIZE, min.y + CHUNK_SIZE, min.z + CHUNK_SIZE);
        constexpr float SAFETY_MARGIN = CHUNK_SIZE * 2.0f;
        if (!frustum.IntersectsAABB(min - SAFETY_MARGIN, max + SAFETY_MARGIN))
            continue;

        candidates.push_back({coord, ScoreChunk(min, cameraPos, forward, velocity)});
    }

    std::ranges::sort(candidates,
                      [](const ScoredChunk& a, const ScoredChunk& b) {
                          return a.priority > b.priority;
                      });

    int sentCount = 0;
    for (const auto& req : candidates) {
        if (world_.TryRequestChunk(req.coord, netClient_))
            sentCount++;
    }

    // Log loading rate every tick — shows how many new chunks we're requesting
    if (sentCount > 0 || !candidates.empty()) {
        spdlog::trace("load_run cam=({:.0f},{:.0f},{:.0f}) candidates={} sent={} pending={} loaded={}",
                      cameraPos.x, cameraPos.y, cameraPos.z,
                      candidates.size(), sentCount,
                      world_.PendingCount(), world_.ChunkCount());
    }

    EvictFarChunks(cameraPos);
}

void ChunkLoadManager::EvictFarChunks(glm::vec3 cameraPos) {
    if (world_.ChunkCount() <= maxChunks_)
        return;

    struct DistEntry { float distSq; ChunkCoord coord; };
    std::vector<DistEntry> entries;
    entries.reserve(world_.ChunkCount());

    world_.ForEachLoadedChunk([&](ChunkCoord coord) {
        glm::vec3 center(coord.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f,
                          coord.y * CHUNK_SIZE + CHUNK_SIZE * 0.5f,
                          coord.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
        glm::vec3 diff = center - cameraPos;
        entries.push_back({glm::dot(diff, diff), coord});
    });

    std::ranges::sort(entries,
                      [](const DistEntry& a, const DistEntry& b) { return a.distSq > b.distSq; });

    size_t excess = world_.ChunkCount() > maxChunks_ ? world_.ChunkCount() - maxChunks_ : 0;
    size_t evictCount = std::min(excess, entries.size());

    for (size_t i = 0; i < evictCount; ++i) {
        uint64_t key = MakeChunkKey(entries[i].coord);

        // If already in eviction queue, skip (don't re-add). The TTL from the
        // first push will eventually drain it.
        if (evictionQueue_.find(key) != evictionQueue_.end())
            continue;

        // Soft eviction: add to TTL queue instead of immediate eviction.
        // The chunk stays loaded until ProcessEvictionQueue decides to evict.
        evictionQueue_[key] = EVICTION_TTL;
        netClient_.SendPlayerAction(0, Protocol::PlayerActionType_UNLOAD,
                                     entries[i].coord.x * 32, 0, entries[i].coord.z * 32);
    }
}

void ChunkLoadManager::ProcessEvictionQueue(float dt) {
    if (evictionQueue_.empty())
        return;

    for (auto it = evictionQueue_.begin(); it != evictionQueue_.end(); ) {
        it->second -= dt;
        if (it->second <= 0) {
            // TTL expired — actually evict
            ChunkCoord c;
            c.x = static_cast<int32_t>((it->first >> 42) & 0x1FFFFF) - CHUNK_KEY_BIAS;
            c.y = static_cast<int32_t>((it->first >> 21) & 0x1FFFFF) - CHUNK_KEY_BIAS;
            c.z = static_cast<int32_t>(it->first & 0x1FFFFF) - CHUNK_KEY_BIAS;
            world_.EvictChunk(c);
            it = evictionQueue_.erase(it);
        } else {
            ++it;
        }
    }
}

float ChunkLoadManager::ScoreChunk(glm::vec3 min, glm::vec3 cameraPos,
                                    glm::vec3 forward, glm::vec3 velocity) {
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
