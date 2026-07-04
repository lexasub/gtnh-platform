#pragma once
#include <cstdint>
#include <vector>

namespace simcore {

struct ChunkCoord {
  int32_t x, y, z;
};

class ChunkSnapshot {
public:
  ChunkSnapshot(const uint8_t *data, size_t len);

  ChunkCoord coord() const;
  uint16_t getBlock(uint32_t x, uint32_t y, uint32_t z) const;
  uint8_t getMeta(uint32_t x, uint32_t y, uint32_t z) const;
  uint32_t getMultiblock(uint32_t x, uint32_t y, uint32_t z) const;
  size_t size() const;
  const uint8_t *data() const;

private:
  std::vector<uint8_t> data_;
  ChunkCoord coord_;
  std::vector<uint16_t> blocks_;
  std::vector<uint8_t> meta_;
  std::vector<uint32_t> multiblock_;
};

} // namespace simcore