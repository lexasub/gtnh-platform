#pragma once

#include <memory>
#include <vector>
#include "../Common/BlockVertex.h"

class ChunkNeighborCache;
class ChunkView;

class ChunkMeshBuilder {
public:
    struct MeshData {
        std::vector<BlockVertex> vertices;
        std::vector<uint16_t> indices;
    };

    static MeshData Build(const ChunkNeighborCache& cache, std::shared_ptr<const ChunkView> chunk);
};
