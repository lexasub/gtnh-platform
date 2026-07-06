#pragma once

#include "../Storage/IBlockStore.h"
#include <functional>
#include <memory>

#include "services/chunk_store/Storage/ChunkStore.h"//TODO don't include for ChunkStore::ChunkCallback

class ChunkStore;      // forward
class WorldGenerator;  // forward
class GenerationQueue; // forward

class ServerWorld {
public:
  ServerWorld();
  ~ServerWorld();

  void Init(int worldId, std::string dbPath, size_t cache_size = 1024,
            size_t max_map_size = 256ULL * 1024 * 1024 * 1024); // 256 GB

  // Синхронные (из IBlockStore, для совместимости)
  void SetBlock(BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId);
  uint16_t GetBlockAt(BlockPos pos) const;
  void SaveChunk(const Chunk &chunk, const ChunkCoord &coord);
  const Chunk *GetChunk(const ChunkCoord &coord); // синхронный (опасно)

  // Асинхронные (callback-based)
  void SetBlockAsync(BlockPos pos, uint16_t blockId, uint8_t meta,
                     uint32_t mbId,
                     std::function<void(bool)> callback = nullptr);
  void AsyncGetChunk(ChunkCoord coord,
                     ChunkStore::ChunkCallback callback);

  /// CALLBACK INVARIANT: Chunk* validity contract
  /// The Chunk* pointer returned to the callback is valid ONLY during the
  /// synchronous invocation of the callback. The callback MUST NOT:
  ///   - Store the pointer for later use
  ///   - Use the pointer after returning
  ///   - Assume the Chunk remains accessible beyond callback invocation
  /// If the Chunk data needs to be retained after the callback returns,
  /// the callback MUST make its own copy of the required data.
  ///
  /// Design note: Chunk* is passed by const reference; callbacks that need
  /// to retain data should copy it (e.g., for UI rendering or serialization).

  ChunkStore *GetChunkStore() const;

private:
  int worldId_ = 0;
  std::unique_ptr<IBlockStore> block_store_; // реально ChunkStore*
  std::unique_ptr<WorldGenerator> generator_;
  std::unique_ptr<GenerationQueue> gen_queue_;

  ChunkStore *chunkStore_ = nullptr;
};