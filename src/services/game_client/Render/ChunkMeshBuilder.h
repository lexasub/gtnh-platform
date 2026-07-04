#pragma once

#include "../Common/BlockVertex.h"
#include <memory>
#include <vector>

class ChunkNeighborCache;
class ChunkView;

class ChunkMeshBuilder {
public:
  struct MeshData {
    std::vector<BlockVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<BlockVertex> transparentVertices;
    std::vector<uint16_t> transparentIndices;
  };

  static MeshData Build(const ChunkNeighborCache &cache,
                        std::shared_ptr<const ChunkView> chunk);
};
