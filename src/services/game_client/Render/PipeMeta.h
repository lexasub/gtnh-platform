#pragma once

#include <cstdint>

// Connection face mask. Bit i of a block's meta byte means face i is connected.
using FaceMask = uint8_t;

constexpr FaceMask FACE_DOWN = 1 << 0;
constexpr FaceMask FACE_UP = 1 << 1;
constexpr FaceMask FACE_NORTH = 1 << 2;
constexpr FaceMask FACE_SOUTH = 1 << 3;
constexpr FaceMask FACE_WEST = 1 << 4;
constexpr FaceMask FACE_EAST = 1 << 5;

// Direction index → world offset. Convention: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
// Opposite face = index ^ 1 (0↔1, 2↔3, 4↔5).
constexpr int32_t DX[6] = {1, -1, 0, 0, 0, 0};
constexpr int32_t DY[6] = {0, 0, 1, -1, 0, 0};
constexpr int32_t DZ[6] = {0, 0, 0, 0, 1, -1};

// Meta bit i (face i) → FaceMask. bit0=+X→FACE_EAST, bit1=-X→FACE_WEST,
// bit2=+Y→FACE_UP, bit3=-Y→FACE_DOWN, bit4=+Z→FACE_SOUTH, bit5=-Z→FACE_NORTH.
constexpr FaceMask META_BIT_TO_FACEMASK[6] = {FACE_EAST, FACE_WEST, FACE_UP,
                                              FACE_DOWN, FACE_SOUTH, FACE_NORTH};

// Convert a connection-mask meta byte to a FaceMask of connected faces.
// meta==0 (legacy/unset) MUST be treated as all 6 connected by the caller
// (returns 0 here; callers normalize 0 → 0x3F before/after).
inline FaceMask metaToFaceMask(uint8_t m) {
  FaceMask r = 0;
  for (int i = 0; i < 6; ++i)
    if (m & (1 << i)) r |= META_BIT_TO_FACEMASK[i];
  return r;
}
