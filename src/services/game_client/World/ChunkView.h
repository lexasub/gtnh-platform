#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Chunk/Chunk.h"

// Compressed-palette-backed chunk data.
// Constructor takes palette-encoded chunk (~2 KB) and decodes on first access.
// GetBlock is O(1) after the lazy full decode.
class ChunkView {
public:
  explicit ChunkView(std::shared_ptr<std::vector<uint8_t>> compressed);

  ~ChunkView();

  uint16_t GetBlock(int x, int y, int z) const;
  void SetBlock(int x, int y, int z, uint16_t block_id, uint8_t meta,
                uint32_t mb_id) const;

  const uint16_t *blocks_data() const;
  const uint8_t *meta_data() const;
  const uint32_t *multiblock_data() const;

  size_t blocks_size() const { return Chunk::VOLUME; }
  size_t meta_size() const { return Chunk::VOLUME; }
  size_t multiblock_size() const { return Chunk::VOLUME; }

private:
  std::shared_ptr<std::vector<uint8_t>> compressed_;
  mutable std::unique_ptr<Chunk> flat_; // lazily decoded on first block access

  void ensureFlat() const;
};
