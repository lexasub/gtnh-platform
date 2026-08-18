#pragma once
#include "ChunkMeshBuilder.h"
#include "PipeMeta.h"
#include "common/ItemId.h"
#include <cstddef>
#include <cstdint>
#include <functional>

enum class PipeType : uint8_t {
  // Order MUST match data/registry/items.csv pipe ids (1111:10:0..4):
  // 0=fluid_pipe, 1=item_pipe, 2=dense_item_pipe, 3=dense_fluid_pipe, 4=heat_pipe.
  // blockIdToPipeType / pipeTypeToBlockId rely on this positional mapping.
  FLUID_PIPE,
  ITEM_PIPE,
  DENSE_ITEM_PIPE,
  DENSE_FLUID_PIPE,
  HEAT_PIPE,
  CABLE_TIN,
  CABLE_COPPER,
  CABLE_GOLD,
  CABLE_ALU,
  CABLE_TUNGSTEN,
  CABLE_PLATINUM,
};

// FaceMask and FACE_* are defined in PipeMeta.h.

// Check if PipeType is a cable variant (not a pipe)
inline bool isCableType(PipeType type) {
  return type >= PipeType::CABLE_TIN;
}

// Map PipeType cable to voltage tier (1=LV/tin … 6=platinum)
// Only valid when isCableType(type) is true
inline uint8_t pipeTypeToCableTier(PipeType type) {
  return static_cast<uint8_t>(type) - static_cast<uint8_t>(PipeType::CABLE_TIN) + 1;
}

// Returns the WORLD block id for a pipe/cable type. These must match the ids
// actually stored in chunks (data/registry/items.csv):
//   pipes:  1111:10:0..4  -> 0xF800..0xF804
//   cables: 1111:01:0..5  -> 0xF400..0xF405
// This is the exact inverse of blockIdToPipeType / blockIdToCableTier, which is
// what detectConnections relies on to match neighbouring pipe/cable blocks.
inline uint16_t pipeTypeToBlockId(PipeType type) {
  if (isCableType(type)) {
    uint8_t tier = pipeTypeToCableTier(type); // 1..6
    return static_cast<uint16_t>(ItemId::pack("1111:01:0") + (tier - 1));
  }
  return static_cast<uint16_t>(ItemId::pack("1111:10:0") +
                               static_cast<int>(type));
}

// Cable tier → RGBA color for cable-specific rendering
// Returns pointer to 4 uint8_t values [R, G, B, A]
inline const uint8_t* cableTierColor(uint8_t tier) {
  static constexpr uint8_t CABLE_TIER_COLORS[6][4] = {
    {183, 115,  51, 255}, // 1 LV  tin       #B77333
    {217, 166,  33, 255}, // 2 MV  gold      #D9A621
    {102, 102, 102, 255}, // 3 HV  tungsten  #666666
    {153, 153, 204, 255}, // 4 EV  platinum  #9999CC
    {255, 204,  51, 255}, // 5 IV  (alu)     #FFCC33
    { 51, 153, 255, 255}, // 6 LuV (platin)  #3399FF
  };
  if (tier < 1 || tier > 6) tier = 1;
  return CABLE_TIER_COLORS[tier - 1];
}

class PipeMeshBuilder {
public:
  PipeMeshBuilder() = default;
  FaceMask detectConnections(
      int32_t x, int32_t y, int32_t z, PipeType type,
      std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock,
      std::function<uint8_t(int32_t, int32_t, int32_t)> getMeta = nullptr);
  ChunkMeshBuilder::MeshData buildPipeMesh(int32_t x, int32_t y, int32_t z,
                                           PipeType type, FaceMask connections);
};
