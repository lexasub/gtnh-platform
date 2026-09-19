// chunk.h — only this, nothing else
#pragma once

#include <array>
#include <cstdint>
#include <new>

struct alignas(64) Chunk { //TODO research - may be write such in SectionCodec (use sections for better brefetching) - do it only if in other places we may do it optimal
  static constexpr int SIZE = 32;
  static constexpr int VOLUME = SIZE * SIZE * SIZE;
  // TODO bool hasBlocks()
  alignas(64) mutable std::array<uint16_t, VOLUME> blocks; // 0 = air
  //now chunk used only for worldgen, in current time worldgen don't fill meta and multiblock
  //alignas(64) mutable std::array<uint8_t, VOLUME> meta;    // orientation, mb_id
                                                           // lower 8 bits
  /*alignas(64) mutable std::array<uint32_t,
                                 VOLUME> multiblock;*/ // full mb_id, 0 = free
  // TODO(diff-protocol): uint64_t version = 0;
  //   Monotonically increasing revision on the server.
  //   Client uses it to:
  //     - ignore stale ChunkData
  //     - know that its pending change is committed
  //     - detect concurrent mutations from other players

  // Access to blocks array for bulk operations (ore generation)
  std::array<uint16_t, VOLUME> &getBlocks() const { return blocks; }

  Chunk() = default;
  ~Chunk() = default;

  Chunk(const Chunk &) = delete;
  Chunk &operator=(const Chunk &) = delete;

  Chunk(Chunk &&) = default;
  Chunk &operator=(Chunk &&) = default;

  static Chunk construct() { return {}; }
  static void construct(Chunk *ptr) {
    if (ptr)
      new (ptr) Chunk{};
  }
};