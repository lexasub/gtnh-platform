// Wrench-grid zone mapping tests. The nine-grid overlay and the wrench click
// hit-test must AGREE: the cell the overlay draws an X in for a connected
// world face must be the cell whose world (u,v) determineWrenchingSide maps
// back to that same face. If they diverge (as they did with the wire/cube
// index mix-up), crosses appear on the wrong side of the pipe. These tests
// assert the agreement for every faced face and every grid cell, plus the
// structural match to GT5U's GRID_SWITCH_TABLE — all without launching the
// game.
#include <cstdio>
#include <cstdint>

#include <glm/glm.hpp>

#include "Render/WrenchGrid.h"
#include "World/WrenchingSide.h"

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    ++g_tests;
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)

// GT5U BlockOverlayRenderer.GRID_SWITCH_TABLE (reference, wire order
// {DOWN,UP,NORTH,SOUTH,WEST,EAST}): row = faced side, col = connected side.
// Position codes: 0 = centre (faced), 5 = outer corners (back face), 1..4 =
// the four edge strips. Verified against GT5-Unofficial master + the 2017
// original (commit c89acf537d).
static const int kGtGridSwitch[6][6] = {
    {0, 5, 3, 1, 2, 4}, {5, 0, 1, 3, 2, 4},
    {1, 3, 0, 5, 2, 4}, {3, 1, 5, 0, 2, 4},
    {4, 2, 3, 1, 0, 5}, {2, 4, 3, 1, 5, 0},
};

// Collapse our 3x3 cell to a GT-style position code so the tables can be
// compared structurally. (The exact 1..4 edge numbering differs from GT's —
// it is camera-independent here — but the *shape* must match: centre, corners
// for the back face, four distinct edges.)
static int toGtCode(uint8_t cell) {
    switch (cell) {
    case 4:   return 0;   // centre
    case 0xFF: return 5;  // far face -> outer corners
    case 1:   return 1;   // v=0 edge
    case 3:   return 2;   // u=0 edge
    case 5:   return 3;   // u=1 edge
    case 7:   return 4;   // v=1 edge
    default:  return -1;
    }
}

// For each faced face, its 4 corners must cover exactly the 4 unit-square
// (u,v) corners — otherwise the overlay's canonical corner ordering is not a
// bijection and the grid would be mirrored on some face.
static void test_corner_uv_are_unit_square() {
    for (int faced = 0; faced < 6; ++faced) {
        bool seen[2][2] = {};
        for (int i = 0; i < 4; ++i) {
            const glm::vec2 uv =
                wrench_grid::cornerUV((uint8_t)faced, wrench_grid::kFaceCorners[faced][i]);
            const int cu = (uv.x > 0.5f) ? 1 : 0;
            const int cv = (uv.y > 0.5f) ? 1 : 0;
            seen[cu][cv] = true;
        }
        const bool ok = seen[0][0] && seen[0][1] && seen[1][0] && seen[1][1];
        CHECK(ok, "faced corners cover all four (u,v) unit-square corners");
    }
}

// zoneOf invariants: faced -> centre cell 4, far -> corners (0xFF), the 4 side
// faces -> 4 distinct edge cells.
static void test_zone_assignments() {
    for (int faced = 0; faced < 6; ++faced) {
        CHECK_EQ((int)wrench_grid::zoneOf((uint8_t)faced, (uint8_t)faced), 4,
                 "faced face at centre");
        CHECK_EQ((int)wrench_grid::zoneOf((uint8_t)faced, (uint8_t)(faced ^ 1)), 0xFF,
                 "far face at corners");
        bool edge[9] = {};
        for (int f = 0; f < 6; ++f) {
            if (f == faced || f == (faced ^ 1)) continue;
            const uint8_t cell = wrench_grid::zoneOf((uint8_t)faced, (uint8_t)f);
            CHECK(cell != 0xFF && cell != 4, "side face -> single non-centre cell");
            CHECK(cell % 3 == 0 || cell % 3 == 2 || cell / 3 == 0 || cell / 3 == 2,
                  "side cell is on a grid edge");
            edge[cell] = true;
        }
        int count = 0;
        for (int i = 0; i < 9; ++i) if (edge[i]) ++count;
        CHECK_EQ(count, 4, "four side faces map to four distinct edge cells");
    }
}

// Structural match with GT5U GRID_SWITCH_TABLE: same centre / corners positions
// and four distinct edge positions per row.
static void test_matches_gt_shape() {
    for (int fw = 0; fw < 6; ++fw) {
        const uint8_t faced = wrench_grid::kWireToCube[(uint8_t)fw];
        int row[6];
        for (int cw = 0; cw < 6; ++cw) {
            const uint8_t cell = wrench_grid::zoneOf(faced, wrench_grid::kWireToCube[(uint8_t)cw]);
            row[cw] = toGtCode(cell);
        }
        CHECK_EQ(row[fw], 0, "faced side -> centre");
        CHECK_EQ(row[fw ^ 1], 5, "far side -> corners");
        bool seen[5] = {};
        for (int cw = 0; cw < 6; ++cw) {
            if (cw == fw || cw == (fw ^ 1)) continue;
            CHECK(row[cw] >= 1 && row[cw] <= 4, "side face -> an edge code");
            seen[row[cw]] = true;
        }
        CHECK(seen[1] && seen[2] && seen[3] && seen[4],
              "four side faces -> four distinct edge codes (GT shape)");
    }
}

// THE key guarantee: the cell the overlay draws for world face F is the cell
// whose world (u,v) determineWrenchingSide classifies as F. Sample every cell
// at its centre and cross-check the overlay mapping against the click mapping.
static void test_display_matches_click() {
    for (int faced = 0; faced < 6; ++faced) {
        // cell -> world face (cube order) inverse of zoneOf.
        int faceOfCell[9];
        for (int c = 0; c < 9; ++c) faceOfCell[c] = -1;
        for (int f = 0; f < 6; ++f) {
            if (f == (faced ^ 1)) {
                for (int c : wrench_grid::kCornerCells) faceOfCell[c] = f;
            } else {
                faceOfCell[wrench_grid::zoneOf((uint8_t)faced, (uint8_t)f)] = f;
            }
        }
        for (int cell = 0; cell < 9; ++cell) {
            const glm::vec2 uv = wrench_grid::cellUV((uint8_t)cell);
            const uint8_t wireGot = determineWrenchingSide(
                wrench_grid::kCubeToWire[(uint8_t)faced], uv.x, uv.y);
            const uint8_t cubeGot = wrench_grid::kWireToCube[wireGot];
            char buf[96];
            snprintf(buf, sizeof(buf), "faced=%d cell=%d: overlay face %d, click -> %d",
                     faced, cell, faceOfCell[cell], (int)cubeGot);
            CHECK_EQ((int)cubeGot, faceOfCell[cell], buf);
        }
    }
}

// GTNH boundary semantics: hit coords quantize to 1/16 on MP, so the >= 0.75
// (not >) is load-bearing — exactly 0.75 must select the edge face.
static void test_half_threshold_boundary() {
    // SOUTH face (wire 3): u=X (west->east), v=Y.
    CHECK_EQ((int)determineWrenchingSide(3, 0.75f, 0.5f), 5, "u=0.75 -> EAST (>= 0.75)");
    CHECK_EQ((int)determineWrenchingSide(3, 0.749f, 0.5f), 3, "u=0.749 -> centre");
    CHECK_EQ((int)determineWrenchingSide(3, 0.249f, 0.5f), 4, "u=0.249 -> WEST");
    CHECK_EQ((int)determineWrenchingSide(3, 0.25f, 0.5f), 3, "u=0.25 -> centre (strict <)");
}

// Regression: the raycast face normal -> wire side conversion must NOT be
// flipped. Raycaster::RaycastHit returns the OUTWARD normal of the ENTERED
// face (-lastStep); an early version mapped it backwards (faceY==-1 -> UP),
// mirroring the whole grid so clicking the visible face toggled the far one.
static void test_face_normal_to_wire_side() {
    CHECK_EQ((int)faceNormalToWireSide(0, -1, 0), 0, "-Y normal -> DOWN");
    CHECK_EQ((int)faceNormalToWireSide(0,  1, 0), 1, "+Y normal -> UP");
    CHECK_EQ((int)faceNormalToWireSide(0,  0, -1), 2, "-Z normal -> NORTH");
    CHECK_EQ((int)faceNormalToWireSide(0,  0,  1), 3, "+Z normal -> SOUTH");
    CHECK_EQ((int)faceNormalToWireSide(-1, 0, 0), 4, "-X normal -> WEST");
    CHECK_EQ((int)faceNormalToWireSide( 1, 0, 0), 5, "+X normal -> EAST");
    // GTNH flow guarantee: a centre-click on the entered face resolves back
    // to that same face for all six faces.
    for (int wire = 0; wire < 6; ++wire) {
        const glm::vec3 n =
            wrench_grid::kFaceNormal[wrench_grid::kWireToCube[(uint8_t)wire]];
        const uint8_t got = determineWrenchingSide(
            faceNormalToWireSide((int)n.x, (int)n.y, (int)n.z), 0.5f, 0.5f);
        char buf[96];
        snprintf(buf, sizeof(buf), "centre-click on entered face %d -> %d", wire, got);
        CHECK_EQ((int)got, wire, buf);
    }
}

int main() {
    test_corner_uv_are_unit_square();
    test_zone_assignments();
    test_matches_gt_shape();
    test_display_matches_click();
    test_half_threshold_boundary();
    test_face_normal_to_wire_side();
    printf("%d/%d wrench-grid checks passed\n", g_passed, g_tests);
    return g_failed ? 1 : 0;
}
