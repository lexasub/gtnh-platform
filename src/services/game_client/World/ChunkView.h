#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// Forward-declare: MutableChunk is heavy, avoid include in header.
struct MutableChunk;

class ChunkView {
public:
  explicit ChunkView(std::shared_ptr<std::vector<uint8_t>> wire_data);

  ~ChunkView();

  uint16_t GetBlock(int x, int y, int z) const;
  void SetBlock(int x, int y, int z, uint16_t block_id, uint8_t meta,
                uint32_t mb_id) const;

  const uint16_t *blocks_data() const;
  const uint8_t *meta_data() const;
  const uint32_t *multiblock_data() const;

  static constexpr int VOLUME = 32 * 32 * 32;
  size_t blocks_size() const { return VOLUME; }
  size_t meta_size() const { return VOLUME; }
  size_t multiblock_size() const { return VOLUME; }

private:
  mutable std::shared_ptr<std::vector<uint8_t>> wire_data_;
  mutable std::unique_ptr<MutableChunk> flat_;

  // Lazy flat-export buffers for rendering.
  mutable std::unique_ptr<uint16_t[]> flat_blocks_;
  mutable std::unique_ptr<uint8_t[]> flat_meta_;
  mutable std::unique_ptr<uint32_t[]> flat_mb_;

  void ensureFlat() const;
  void ensureFlatArrays() const;
};
