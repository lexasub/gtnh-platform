#pragma once
#include "ChunkMeshBuilder.h"
#include "PipeMeshBuilder.h"
#include <cstdint>

class CableMeshBuilder {
public:
  CableMeshBuilder() = default;
  FaceMask detectConnections(
      int32_t x, int32_t y, int32_t z, uint8_t tier,
      std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock);
  ChunkMeshBuilder::MeshData buildCableMesh(int32_t x, int32_t y, int32_t z,
                                            uint8_t tier, FaceMask connections);
};
