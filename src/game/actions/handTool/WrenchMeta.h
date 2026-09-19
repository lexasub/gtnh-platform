#pragma once

#include <cstdint>

// Pipe/cable connection toggle math, shared by WrenchActionHandler and its tests.
//
// Wire face convention (matches GatewayMsg / client TargetFace):
//   0=DOWN, 1=UP, 2=NORTH, 3=SOUTH, 4=WEST, 5=EAST
// Meta layout: bit i connects face i, index order {+X,-X,+Y,-Y,+Z,-Z}.
// meta == 0 means "all connected" (0x3F). Opposite face = index ^ 1.

namespace simcore {

inline constexpr int kWrenchFaceDX[6] = {0, 0, 0, 0, -1, 1};
inline constexpr int kWrenchFaceDY[6] = {-1, 1, 0, 0, 0, 0};
inline constexpr int kWrenchFaceDZ[6] = {0, 0, -1, 1, 0, 0};

struct WrenchMetaResult {
    uint8_t hostMeta;
    uint8_t neighborMeta;
};

// Toggle the connection on `wireFace` between a host block (meta = hostMeta)
// and its neighbor across that face (meta = neighborMeta). Returns the new
// meta bytes for both blocks. A meta of 0 is normalized to 0x3F first.
inline WrenchMetaResult computePipeToggle(uint8_t wireFace, uint8_t hostMeta,
                                          uint8_t neighborMeta) {
    if (wireFace > 5) return {hostMeta, neighborMeta};
    int dir = -1;
    if      (kWrenchFaceDX[wireFace] == 1)  dir = 0;  // +X
    else if (kWrenchFaceDX[wireFace] == -1) dir = 1;  // -X
    else if (kWrenchFaceDY[wireFace] == 1)  dir = 2;  // +Y
    else if (kWrenchFaceDY[wireFace] == -1) dir = 3;  // -Y
    else if (kWrenchFaceDZ[wireFace] == 1)  dir = 4;  // +Z
    else if (kWrenchFaceDZ[wireFace] == -1) dir = 5;  // -Z
    const uint8_t bit = static_cast<uint8_t>(1u << dir);
    const uint8_t opp = static_cast<uint8_t>(1u << (dir ^ 1));
    const uint8_t mHb = (hostMeta == 0) ? 0x3F : hostMeta;
    const uint8_t mNb = (neighborMeta == 0) ? 0x3F : neighborMeta;
    return {static_cast<uint8_t>(mHb ^ bit), static_cast<uint8_t>(mNb ^ opp)};
}

}  // namespace simcore
