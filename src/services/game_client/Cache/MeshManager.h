#pragma once

#include "Cache/ChunkMeshCache.h"
#include "Common/Types.h"
#include "Render/ChunkMeshProvider.h"
#include <atomic>
#include <memory>
#include <tbb/global_control.h>
#include <tbb/task_group.h>

class World;
class ChunkView;

// Manages chunk mesh lifecycle: triggers async TBB mesh builds when block
// updates or chunk data arrives, owns the ChunkMeshCache and the provider
// that RenderLib uses for visible-mesh iteration.
//
// Thread-safe: OnBlockUpdate / OnChunkData may be called from any thread
// (asio network callbacks). ProcessPendingOps() must be called on the main
// thread (bgfx thread) to apply completed builds to GPU.
class MeshManager {
public:
  explicit MeshManager(World &world);
  ~MeshManager();

  MeshManager(const MeshManager &) = delete;
  MeshManager &operator=(const MeshManager &) = delete;

  ChunkMeshProvider *GetProvider() const { return meshProvider_.get(); }
  ChunkMeshCache &Cache() { return meshCache_; }

  // Called when server sends a BlockUpdate message.
  void OnBlockUpdate(BlockPos pos, uint16_t block_id, uint8_t meta,
                     uint32_t mb_id, World &world);

  // Called when server responds with a full chunk snapshot.
  void OnChunkData(ChunkCoord coord, std::shared_ptr<ChunkView> chunk,
                   World &world);

  // Apply completed mesh builds to GPU (main thread only).
  void ProcessPendingOps();

  // Enqueue GPU mesh destruction for evicted chunk.
  void HandleEviction(const ChunkCoord &coord);

  // Invalidate all GPU handles after bgfx::shutdown (skip destroy).
  void DiscardHandles();

  size_t MeshCount() const { return meshCache_.Size(); }
  void WaitForPending() { meshBuildGroup_.wait(); }
  void RequestShutdown() { shuttingDown_ = true; }

private:
  static uint64_t HashChunkData(const uint16_t *blocks, size_t count);

  ChunkMeshCache meshCache_;
  tbb::global_control tbbControl_{tbb::global_control::max_allowed_parallelism,
                                  2};
  tbb::task_group meshBuildGroup_;
  std::unique_ptr<ChunkMeshProvider> meshProvider_;
  std::atomic<bool> shuttingDown_{false};
};
