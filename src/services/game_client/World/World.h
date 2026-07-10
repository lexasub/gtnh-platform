#pragma once

#include "../RenderLib/Common/IBlockQuery.h"
#include "ChunkStorage.h"
#include <glm/glm.hpp>
#include <memory>
#include <mutex>
#include <oneapi/tbb/concurrent_hash_map.h>
#include <oneapi/tbb/concurrent_unordered_set.h>
#include <tbb/concurrent_unordered_set.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class NetClient;
struct Frustum;

// Manages chunk loading/unloading, provides block queries, integrates with
// network.
class World : public IBlockQuery {
public:
  static constexpr int VIEW_RADIUS = 8; // chunks in each direction
  static constexpr size_t MAX_CHUNKS = 1024;

  // TODO(diff-protocol): compile-time option — how we handle block placement
  // feedback:
  //   #define WORLD_FAST_SEND       — send PlayerAction immediately, no waiting
  //   #define WORLD_OPTIMISTIC_AFTER — apply pending block locally before
  //   server ack #define WORLD_SERVER_PRIORITY — server values always win over
  //   pending

  World();
  ~World() override;

  void GetNewChunksFromServer(const Frustum &frustum, glm::vec3 cameraPos,
                              glm::vec3 forward, glm::vec3 velocity,
                              NetClient &netClient);

  // IBlockQuery
  uint16_t GetBlockAt(BlockPos pos) const override;

  // Chunk access for rendering
  std::shared_ptr<const ChunkView> GetChunk(const ChunkCoord &c) const;

  // Evict a single chunk (removes from storage and pending set)
  void EvictChunk(const ChunkCoord &coord);

  // Atomically check + mark pending, then send request. Returns true if request
  // was sent.
  bool TryRequestChunk(const ChunkCoord &coord, class NetClient &netClient);

  // Iterate all loaded chunk coords
  template <typename F> void ForEachLoadedChunk(F &&fn) const {
    storage_.ForEachCoord(
        [&](ChunkCoord coord, const std::shared_ptr<const ChunkView> &) {
          fn(coord);
        });
  }

  // Called by NetClient when chunk data arrives
  std::shared_ptr<const ChunkView> OnChunkData(std::shared_ptr<ChunkView> chunk,
                                               const ChunkCoord &coord);

  // Called when server sends an incremental diff (BlockUpdate).
  // Applies the change directly to the chunk (server data wins).
  void OnBlockUpdate(BlockPos pos, uint16_t block_id, uint8_t meta,
                     uint32_t mb_id);

  // Called when server confirms our PlayerAction is committed (ACCEPTED or
  // CONFLICT). Removes the position from pending block actions.
  void ClearBlockActionPending(BlockPos pos);

  // Returns true if a block action is in-flight for this position (debounce).
  bool IsBlockActionPending(BlockPos pos) const;

  // Removes pending block actions older than |timeout|.
  void ClearExpiredBlockActions(std::chrono::steady_clock::duration timeout);

  // Marks a position as having an in-flight block action.
  void MarkBlockActionSent(BlockPos pos);

  size_t ChunkCount() const { return storage_.Size(); }

  bool HasChunk(const ChunkCoord &coord) const;
  // Returns true if coord is in pendingRequests_ (chunk requested but not yet
  // received).
  bool IsPending(const ChunkCoord &coord) const;

  std::vector<ChunkCoord> TakeEvictedChunks();

  static float calcCandidateScore(glm::vec3 vec, glm::vec3 vec3,
                                  glm::vec3 forward, glm::vec3 velocity);

private:
  void EvictFarChunks(glm::vec3 cameraPos);

  struct Uint64Hash {
    static size_t operator()(uint64_t key) { return key; }
    static bool operator()(uint64_t a, uint64_t b) { return a == b; }
  };
  using PendingSet = tbb::concurrent_unordered_set<uint64_t, Uint64Hash>;
  using BlockActionMap =
      tbb::concurrent_hash_map<uint64_t, int64_t>; // key → timestamp_ns
  ChunkStorage storage_;
  PendingSet pendingRequests_;         // chunks requested but not yet received
  BlockActionMap pendingBlockActions_; // block positions with in-flight
                                       // break/place actions
  mutable std::mutex pendingEvictedMtx_;
  std::vector<ChunkCoord> pendingEvicted_;

  // Pending block changes: when OnBlockUpdate is applied locally, we save
  // the change here. OnChunkData (fresh snapshot from server) might overwrite
  // it with stale data (see ChunkStore race: readTransaction vs CAS), so we
  // re-apply pending changes over the fresh snapshot.
  struct PendingBlock {
    uint16_t block_id = 0;
    uint8_t meta = 0;
    uint32_t mb_id = 0;
  };
  // chunk key -> (block pos key -> pending block)
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, PendingBlock>>
      pendingChanges_;
};