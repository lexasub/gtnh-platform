#include "ChunkLoadManager.h"
#include "../World/World.h"
#include "../World/ChunkView.h"
#include "../Network/NetClient.h"
#include "core_generated.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <ranges>

ChunkLoadManager::ChunkLoadManager(World& world, NetClient& netClient)
    : world_(world), netClient_(netClient) {
}

void ChunkLoadManager::SetUpdateRate(float updatesPerSecond) {
    updateInterval_ = 1.0f / updatesPerSecond;
}

void ChunkLoadManager::Update(const Frustum& frustum, glm::vec3 cameraPos,
                               glm::vec3 forward, glm::vec3 velocity, float dt) {
    // Rate limit: don't re-scan every frame (10 Hz default)
    timeSinceUpdate_ += dt;
    if (timeSinceUpdate_ < updateInterval_)
        return;
    timeSinceUpdate_ = 0.0f;

    // Full priority re-scan every tick. No movement/rotation short-circuit:
    // the 10 Hz rate limit is enough — skipping scans on small motion is
    // false economy because rotation changes the frustum even when the
    // camera hasn't moved.
    RunLoadPass(frustum, cameraPos, forward, velocity);
}

void ChunkLoadManager::RunLoadPass(const Frustum& frustum, glm::vec3 cameraPos,
                                    glm::vec3 forward, glm::vec3 velocity) {
    int centerX = static_cast<int>(std::floor(cameraPos.x / CHUNK_SIZE));
    int centerY = static_cast<int>(std::floor(cameraPos.y / CHUNK_SIZE));
    int centerZ = static_cast<int>(std::floor(cameraPos.z / CHUNK_SIZE));

    int R = World::VIEW_RADIUS;
    int side = 2 * R + 1;
    int total = side * side * side;

    std::vector<ScoredChunk> candidates;

    for (int idx = 0; idx < total; ++idx) {
        ChunkCoord coord{
            (centerX + (idx / (side * side) - R)),
            (centerY + (idx / side % side - R)),
            (centerZ + (idx % side - R))
        };

        if (world_.HasChunk(coord) || world_.IsPending(coord))
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

    for (const auto& req : candidates) {
        world_.TryRequestChunk(req.coord, netClient_);
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
        world_.EvictChunk(entries[i].coord);
        netClient_.SendPlayerAction(0, Protocol::PlayerActionType_UNLOAD,
                                     entries[i].coord.x * 32, 0, entries[i].coord.z * 32);
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
