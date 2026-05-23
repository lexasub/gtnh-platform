#pragma once
#include <cstdint>
#include "../Chunk/Chunk.h"
#include <common/coords/Coords.h>


class IBlockStore {
 public:
     virtual ~IBlockStore() = default;
     virtual bool HasChunk(ChunkCoord c) const = 0;
     virtual const Chunk* GetChunk(ChunkCoord c) const = 0;
     virtual uint16_t GetBlockAt(BlockPos pos) const = 0;
     virtual void SetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId) = 0;
     virtual bool SaveChunk(const Chunk& chunk, ChunkCoord coord) = 0;
};
