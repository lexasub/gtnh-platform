#pragma once
#include "ChunkMeshBuilder.h"
#include <cstddef>
#include <cstdint>
#include <functional>

enum class PipeType : uint8_t {
  ITEM_PIPE,
  DENSE_ITEM_PIPE,
  FLUID_PIPE,
  DENSE_FLUID_PIPE,
  CABLE_TIN,
  CABLE_COPPER,
  CABLE_GOLD,
  CABLE_ALU,
  CABLE_TUNGSTEN,
  CABLE_PLATINUM,
};

using FaceMask = uint8_t;

constexpr FaceMask FACE_DOWN = 1 << 0;
constexpr FaceMask FACE_UP = 1 << 1;
constexpr FaceMask FACE_NORTH = 1 << 2;
constexpr FaceMask FACE_SOUTH = 1 << 3;
constexpr FaceMask FACE_WEST = 1 << 4;
constexpr FaceMask FACE_EAST = 1 << 5;

inline uint16_t pipeTypeToBlockId(PipeType type) {
  switch (type) {
  // IDs must match data/registry/items.csv:
  //   pipes: fluid=61, item=62, dense_item=64, dense_fluid=65
  //   cables: tin=66, copper=67, gold=68, alu=69, tungsten=70, platinum=71
  case PipeType::ITEM_PIPE:
    return 62;
  case PipeType::DENSE_ITEM_PIPE:
    return 64;
  case PipeType::FLUID_PIPE:
    return 61;
  case PipeType::DENSE_FLUID_PIPE:
    return 65;
  case PipeType::CABLE_TIN:
    return 66;
  case PipeType::CABLE_COPPER:
    return 67;
  case PipeType::CABLE_GOLD:
    return 68;
  case PipeType::CABLE_ALU:
    return 69;
  case PipeType::CABLE_TUNGSTEN:
    return 70;
  case PipeType::CABLE_PLATINUM:
    return 71;
  default:
    return 0;
  }
}

class PipeMeshBuilder {
public:
  PipeMeshBuilder() = default;
  FaceMask detectConnections(
      int32_t x, int32_t y, int32_t z, PipeType type,
      std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock);
  ChunkMeshBuilder::MeshData buildPipeMesh(int32_t x, int32_t y, int32_t z,
                                           PipeType type, FaceMask connections);
};
