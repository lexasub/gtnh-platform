#pragma once

#include <cstdint>
#include <cmath>

// Convert a raycast face normal into the wire face 0..5. Raycaster::RaycastHit
// returns info.faceX/Y/Z = -lastStep: the OUTWARD normal of the face the ray
// ENTERED through (faceY==-1 -> entered through the -Y / DOWN face). Convert
// it directly — an early version flipped all six cases, which mirrored the
// whole grid so a click on the visible face toggled the OPPOSITE face.
inline uint8_t faceNormalToWireSide(int faceX, int faceY, int faceZ) {
    if      (faceY == -1) return 0; // -Y (DOWN)
    else if (faceY ==  1) return 1; // +Y (UP)
    else if (faceZ == -1) return 2; // -Z (NORTH)
    else if (faceZ ==  1) return 3; // +Z (SOUTH)
    else if (faceX == -1) return 4; // -X (WEST)
    else if (faceX ==  1) return 5; // +X (EAST)
    return 0;
}

// GT4/GTCEu-style wrench side hit-testing. Given the face the ray entered
// (sideHit, our wire order: 0=DOWN,1=UP,2=NORTH,3=SOUTH,4=WEST,5=EAST) and the
// LOCAL hit coordinates (0..1 on that face), returns which block face the user
// intends to toggle. This mirrors GT_Utility.determineWrenchingSide / the
// ICoverable 3x3 grid: centre -> the face itself, an edge -> the neighbouring
// face across that edge, a corner -> the OPPOSITE face.
//
// Face axis layout (what u/v mean per face):
//   DOWN/UP   (0,1): u=X (west->east), v=Z (north->south)
//   NORTH/SOUTH (2,3): u=X, v=Y (down->up)
//   WEST/EAST (4,5): u=Z (north->south), v=Y
//
// Returns a wire face 0..5 (0=DOWN,1=UP,2=NORTH,3=SOUTH,4=WEST,5=EAST).
// Mirrors GT5U/GTNH GTUtility.determineWrenchingSide exactly: thresholds are
// 0.25 / >=0.75 (hit coords are quantized to 1/16 in MC), corners -> the
// opposite face.
inline uint8_t determineWrenchingSide(uint8_t sideHit, float u, float v) {
    // Wrap negative/out-of-range coords into [0,1) like the MC frac() call.
    const float nu = (u - std::floor(u));
    const float nv = (v - std::floor(v));
    // Opposite face (wire order pairs 0<->1, 2<->3, 4<->5).
    const uint8_t back = sideHit ^ 1;

    switch (sideHit) {
    case 0: case 1: {                 // DOWN / UP
        if (nu < 0.25f || nu >= 0.75f) {
            if (nv < 0.25f || nv >= 0.75f) return back;   // corner
            return (nu < 0.25f) ? 4 : 5;                  // WEST / EAST edge
        }
        if (nv < 0.25f) return 2;                         // NORTH
        if (nv >= 0.75f) return 3;                        // SOUTH
        return sideHit;                                   // centre
    }
    case 2: case 3: {                 // NORTH / SOUTH
        if (nu < 0.25f || nu >= 0.75f) {
            if (nv < 0.25f || nv >= 0.75f) return back;
            return (nu < 0.25f) ? 4 : 5;                  // WEST / EAST edge
        }
        if (nv < 0.25f) return 0;                         // DOWN
        if (nv >= 0.75f) return 1;                        // UP
        return sideHit;
    }
    case 4: case 5: {                 // WEST / EAST
        if (nu < 0.25f || nu >= 0.75f) {
            if (nv < 0.25f || nv >= 0.75f) return back;
            return (nu < 0.25f) ? 2 : 3;                  // NORTH / SOUTH edge
        }
        if (nv < 0.25f) return 0;                         // DOWN
        if (nv >= 0.75f) return 1;                        // UP
        return sideHit;
    }
    default:
        return sideHit;
    }
}
