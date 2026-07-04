#pragma once

#include "../Common/Types.h"
#include <cstddef>
#include <glm/glm.hpp>

class World;
class NetClient;
struct Frustum;

// L2 cache: priority-scored chunk loading with rate limiting.
// Decides WHICH chunks to request and WHEN, based on camera position,
// view direction, and velocity. Replaces raw per-frame GetNewChunksFromServer.
//
// - Rate-limited internally (10 Hz default)
// - Full re-scan every tick — no movement/rotation short-circuit
// - Evicts farthest chunks first when over capacity
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

  static float ScoreChunk(glm::vec3 chunkMin, glm::vec3 cameraPos,
                          glm::vec3 forward, glm::vec3 velocity);

  World &world_;
  NetClient &netClient_;

  size_t maxChunks_ = 1024;
  float updateInterval_ = 0.1f; // 10 Hz default
  float timeSinceUpdate_ = 0.0f;

  static constexpr float RUN_SPEED_THRESHOLD = 2.0f; // blocks/sec
};
