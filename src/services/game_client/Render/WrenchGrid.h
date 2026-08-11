#pragma once

#include <cstdint>
#include <glm/glm.hpp>

// Pure wrench-grid geometry shared by the GTNH-style nine-grid overlay and its
// tests. The grid is drawn on the FACED face of the highlighted block; each of
// the 3x3 cells belongs to exactly one world face (centre -> the faced face,
// the four corners -> the far face, the four edge cells -> the side faces).
//
// The grid is a 3x3 in the WORLD (u,v) axes of the faced face — the same (u,v)
// that determineWrenchingSide consumes — so the mapping face->cell here and
// the click hit-test can never disagree. Keeping it in its own header lets the
// overlay be tested without launching the game, and gives one source of truth
// for the cube-geometry tables (kFaceCorners / kFaceNormal) that the overlay
// and the raycast hit-test both use.

namespace wrench_grid {

// Cube-face order == meta-bit order {+X,-X,+Y,-Y,+Z,-Z}. This is the order of
// kFaceCorners / kFaceNormal and of the wrenchConnectable[] array, so a single
// index works for all of them.
enum : uint8_t {
    kPX = 0, kNX = 1, kPY = 2, kNY = 3, kPZ = 4, kNZ = 5,
};

// Wire face order (determineWrenchingSide / the raycast): {DOWN,UP,NORTH,
// SOUTH,WEST,EAST}. Conversion tables between the two:
inline constexpr uint8_t kWireToCube[6] = {3, 2, 5, 4, 1, 0};  // D,U,N,S,W,E -> -Y,+Y,-Z,+Z,-X,+X
inline constexpr uint8_t kCubeToWire[6] = {5, 4, 1, 0, 3, 2};  // +X,-X,+Y,-Y,+Z,-Z -> E,W,U,D,S,N

inline uint8_t opposite(uint8_t f) { return f ^ 1; }  // valid in both orders

// 8 cube corners; bit0=x bit1=y bit2=z.
inline constexpr int kCornerX[8] = {0, 1, 0, 1, 0, 1, 0, 1};
inline constexpr int kCornerY[8] = {0, 0, 1, 1, 0, 0, 1, 1};
inline constexpr int kCornerZ[8] = {0, 0, 0, 0, 1, 1, 1, 1};

// Corners of each cube face (cube order), clockwise seen from outside.
inline constexpr int kFaceCorners[6][4] = {
    {1, 3, 7, 5},  // +X
    {0, 2, 6, 4},  // -X
    {2, 3, 7, 6},  // +Y
    {0, 1, 5, 4},  // -Y
    {4, 5, 7, 6},  // +Z
    {0, 1, 3, 2},  // -Z
};
inline const glm::vec3 kFaceNormal[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};

// World (u,v) axes on the faced face, matching determineWrenchingSide:
//   +Z/-Z: u=X (west->east), v=Y (down->up)
//   +X/-X: u=Z (north->south), v=Y
//   +Y/-Y: u=X (west->east), v=Z (north->south)
// Writes axis codes 0/1/2 for X/Y/Z.
inline void uvAxes(uint8_t face, int& uaxis, int& vaxis) {
    switch (face) {
    case kPX: case kNX: uaxis = 2; vaxis = 1; break;  // Z, Y
    case kPY: case kNY: uaxis = 0; vaxis = 2; break;  // X, Z
    default:            uaxis = 0; vaxis = 1; break;  // X, Y
    }
}
// World (u,v) on `faced` of cube corner `corner` (index 0..7).
inline glm::vec2 cornerUV(uint8_t faced, int corner) {
    int ua;
    int va;
    uvAxes(faced, ua, va);
    const int c[3] = {kCornerX[corner], kCornerY[corner], kCornerZ[corner]};
    return glm::vec2(static_cast<float>(c[ua]), static_cast<float>(c[va]));
}

// Grid cell occupied by world face `face` on the grid drawn on the faced face.
// Cells are 3x3, index 3*row+col, col along u (0=left, 2=right), row along v
// (0=start, 2=end). Returns 4 for the faced face (centre), 0xFF for the far
// face (all four corner cells), else the single edge cell of a side face.
inline uint8_t zoneOf(uint8_t faced, uint8_t face) {
    if (face == faced) return 4;
    if (face == opposite(faced)) return 0xFF;
    // Side face: find the edge it shares with the faced face; that edge is a
    // constant u or v = 0 or 1 on the faced face.
    int shared[2] = {-1, -1};
    int k = 0;
    for (int c = 0; c < 4 && k < 2; ++c)
        for (int d = 0; d < 4 && k < 2; ++d)
            if (kFaceCorners[face][c] == kFaceCorners[faced][d]) shared[k++] = kFaceCorners[face][c];
    if (k < 2) return 4;  // degenerate; not a real side face
    const glm::vec2 a = cornerUV(faced, shared[0]);
    const glm::vec2 b = cornerUV(faced, shared[1]);
    if (a.x == b.x) {                 // constant u edge
        const int col = (a.x < 0.5f) ? 0 : 2;
        return static_cast<uint8_t>(3 * 1 + col);
    }
    // constant v edge
    const int row = (a.y < 0.5f) ? 0 : 2;
    return static_cast<uint8_t>(3 * row + 1);
}

// World (u,v) centre of a grid cell — the point the X-cross is drawn at, and
// the sample the click-hit test uses. (col,row) from cell = 3*row+col.
inline glm::vec2 cellUV(uint8_t cell) {
    const int col = cell % 3, row = cell / 3;
    return glm::vec2((col + 0.5f) / 3.0f, (row + 0.5f) / 3.0f);
}

// The four corner cells of the grid (where the far face's X crosses go).
inline constexpr uint8_t kCornerCells[4] = {0, 2, 6, 8};

}  // namespace wrench_grid
