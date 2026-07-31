#pragma once

#include "../Common/Types.h"
#include <cstddef>
#include <glm/glm.hpp>
#include <unordered_map>

class World;
class NetClient;
struct Frustum;

// L2 cache: priority-scored chunk loading with rate limiting.
// Decides WHICH chunks to request and WHEN, based on camera position,
// view direction, and velocity.
//
// Uses soft-eviction with TTL to prevent chunk flickering: when a chunk
// exceeds the capacity cap it's not evicted immediately — instead it
// enters an eviction queue. If the chunk is needed again before TTL
// expires, the eviction is cancelled. This avoids thrashing when the
// player makes small movements.
class ChunkLoadManager {
public:
  ChunkLoadManager(World &world, NetClient &netClient);

  // Call every frame; actual work is rate-limited internally.
  // dt: frame delta time in seconds.
  void Update(const Frustum &frustum, glm::vec3 cameraPos, glm::vec3 forward,
              glm::vec3 velocity, float dt);

  void SetUpdateRate(float updatesPerSecond);
  float UpdateRate() const { return 1.0f / updateInterval_; }

  void SetMaxChunks(size_t max) { maxChunks_ = max; }
  size_t MaxChunks() const { return maxChunks_; }

private:
  struct ScoredChunk {
    ChunkCoord coord;
    float priority;
  };

  void RunLoadPass(const Frustum &frustum, glm::vec3 cameraPos,
                   glm::vec3 forward, glm::vec3 velocity);
  void EvictFarChunks(glm::vec3 cameraPos);
  void ProcessEvictionQueue(float dt);

  static float ScoreChunk(glm::vec3 chunkMin, glm::vec3 cameraPos,
                          glm::vec3 forward, glm::vec3 velocity);

  World &world_;
  NetClient &netClient_;

  size_t maxChunks_ = 1024;
  float updateInterval_ = 0.033f; // ~30 Hz default
  float timeSinceUpdate_ = 0.0f;

  // Soft-eviction queue: key → remaining TTL in seconds.
  // Chunks here are still in storage but slated for eviction.
  // If re-accessed before TTL expires, they're restored.
  std::unordered_map<uint64_t, float> evictionQueue_;
  static constexpr float EVICTION_TTL = 5.0f; // seconds before actual eviction

  static constexpr float RUN_SPEED_THRESHOLD = 2.0f; // blocks/sec
};
