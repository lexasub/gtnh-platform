// Raycaster DDA tests — beads gp-nm51.
//
// gp-nm51: Raycaster::RaycastHit advanced its DDA point RELATIVELY
// (`pz += tMaxZ * dz`) while GetTargetedBlock advanced it ABSOLUTELY
// (`pz = ray.origin.z + tMaxZ * dz`). tMax* already grows by tDelta* every
// step, so the relative form compounds: for a straight -Z ray from z = 8.5
// the point reads 8.0, 6.5, 4.0, 0.5 instead of 8.0, 7.0, 6.0, 5.0. The
// inflated |p - origin| then trips the reach guard while the DDA is still
// nowhere near maxDist, so RaycastHit returns the no-hit sentinel for any
// target past the first cell. GetTargetedBlock was already correct, which is
// why the block highlight and the left-click gate worked out to REACH_DIST
// while the GT-style wrench face selection (RaycastHitAtCenter ->
// hit.u/v -> determineWrenchingSide) silently did not.
//
// WHAT THIS SUITE IS FOR
// Both raycasters are pure functions of (ray, maxDist, an IBlockQuery double),
// and neither one existed under any direct test: gameclient_wrench_grid_test
// and gameclient_wrench_overlay_test cover the grid mapping and the overlay
// maths, and gameclient_interaction_system_test drives the real
// InteractionSystem but only with rays that hit in the adjacent cell. So the
// class of bug "the DDA stops one cell out" was invisible to the whole suite.
// The tests below assert the two properties that actually broke:
//
//   REACH:  a target at every distance from the eye, on every axis, is found.
//   CONSISTENCY: RaycastHit and GetTargetedBlock, given the same world and
//               the same ray, agree on WHICH block was hit. These are two
//               implementations of one algorithm in one file; any divergence
//               is a bug in one of them, so the test asserts the invariant
//               rather than one implementation's answers.
//
// DETERMINISM
// No display, no GPU, no window, no GL context, no world, no network. The
// block query is a flat std::unordered_map double; every ray is built from
// exact arithmetic (0.5-centred cells, unit axes) so no value is near a face
// boundary by accident, and no assertion depends on iteration order.
//
// HARNESS
// The project's own CHECK/TEST macros, NOT GoogleTest: gtest is absent from
// conanfile.txt, CI does not install libgtest-dev, and CI Release builds with
// a global -Werror, so a find_package(GTest QUIET) guard would silently
// UNREGISTER this test instead of failing it. Same shape as
// src/game/client/World/test/InteractionSystem_test.cpp and
// src/game/storage/test/test_player_inventory_store.cpp.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

#include <glm/glm.hpp>

#include <game/client/Common/Types.h>
#include <RenderLib/Common/IBlockQuery.h>
#include <RenderLib/Utils/Raycaster.h>

// ---------------------------------------------------------------------------
// Project harness
// ---------------------------------------------------------------------------

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char *file, int line, const char *expr,
                       const char *msg = nullptr) {
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

#define CHECK(cond, ...)                                                       \
    test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)

#define CHECK_EQ_I(a, b, ...)                                                  \
    test_check(static_cast<int64_t>(a) == static_cast<int64_t>(b), __FILE__,   \
               __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_NEAR(a, b, eps, ...)                                             \
    test_check(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <=    \
                   (eps),                                                     \
               __FILE__, __LINE__, #a " ~= " #b, ##__VA_ARGS__)

#define TEST(name) do { printf("  TEST: %s\n", #name); test_##name(); } while (0)

namespace {

using renderlib::Raycaster;

constexpr int32_t kNoHit = std::numeric_limits<int32_t>::max();

// A world of solid cells, queried by exact coordinate. Non-listed cells are
// air (id 0), which is what IBlockQuery documents.
class MapWorld final : public IBlockQuery {
public:
    uint16_t GetBlockAt(BlockPos pos) const override {
        auto it = cells_.find(std::make_tuple(pos.x, pos.y, pos.z));
        return it == cells_.end() ? 0 : it->second;
    }

    void fill(int32_t x0, int32_t x1, int32_t y0, int32_t y1, int32_t z0,
              int32_t z1, uint16_t id) {
        for (int32_t x = x0; x <= x1; ++x)
            for (int32_t y = y0; y <= y1; ++y)
                for (int32_t z = z0; z <= z1; ++z)
                    cells_[{x, y, z}] = id;
    }

    void set(BlockPos p, uint16_t id) { cells_[{p.x, p.y, p.z}] = id; }

private:
    std::map<std::tuple<int32_t, int32_t, int32_t>, uint16_t> cells_;
};

constexpr uint16_t kStone = 1;

Ray rayFrom(float ox, float oy, float oz, float dx, float dy, float dz) {
    return {glm::vec3(ox, oy, oz), glm::vec3(dx, dy, dz)};
}

// The two public entry points under test.
struct Both {
    BlockPos targeted;
    Raycaster::HitInfo hit;
};

Both cast(const Raycaster &rc, const Ray &ray, float maxDist) {
    return {rc.GetTargetedBlock(ray, maxDist), rc.RaycastHit(ray, maxDist)};
}

bool isHit(BlockPos p) { return p.x != kNoHit; }

// ---------------------------------------------------------------------------
// gp-nm51 — the reach cases the compounding DDA could not serve
// ---------------------------------------------------------------------------

// The ticket's own repro, as a test: a straight ray at a target 4.5 cells away.
static void test_Raycaster_finds_a_target_four_cells_down_the_ray() {
    MapWorld w;
    w.set({0, 0, 4}, kStone);  // 4.5 cells from an eye at z = 8.5
    Raycaster rc(&w);
    const Ray ray = rayFrom(0.5f, 0.5f, 8.5f, 0.0f, 0.0f, -1.0f);

    const Both r = cast(rc, ray, Raycaster::REACH_DIST);
    CHECK(isHit(r.targeted),
          "gp-nm51: GetTargetedBlock (the already-correct absolute DDA) finds "
          "the block 4.5 cells away");
    if (isHit(r.targeted)) {
        CHECK_EQ_I(r.targeted.x, 0, "sanity: x of the hit cell");
        CHECK_EQ_I(r.targeted.y, 0, "sanity: y of the hit cell");
        CHECK_EQ_I(r.targeted.z, 4, "sanity: z of the hit cell");
    }

    // The defect: the relative DDA reads the point 4 cells along the ray as
    // z = 4.0 by a wrong route, then the NEXT step jumps it to z = 0.5, and
    // the inflated distance trips the reach guard before the loop ever visits
    // cell z = 4. It returns the sentinel.
    CHECK(isHit(r.hit.pos),
          "gp-nm51: RaycastHit must reach the same block — the old relative DDA "
          "compounded tMax into the point and gave up before getting here");
    if (isHit(r.hit.pos)) {
        CHECK_EQ_I(r.hit.pos.x, 0, "RaycastHit agrees on x");
        CHECK_EQ_I(r.hit.pos.y, 0, "RaycastHit agrees on y");
        CHECK_EQ_I(r.hit.pos.z, 4, "RaycastHit agrees on z");
        // Entered through the +Z face (the ray travels -Z), so the normal
        // points back at the eye, i.e. +Z. This is what feeds TargetFace and
        // the wrench side selection.
        CHECK_EQ_I(r.hit.faceZ, 1, "the face entered is +Z (facing the eye)");
        CHECK_EQ_I(r.hit.faceX, 0, "no X component on an axis-aligned -Z ray");
        CHECK_EQ_I(r.hit.faceY, 0, "no Y component on an axis-aligned -Z ray");
        // The ray runs down the middle of the cell, so the local hit is
        // (0.5, 0.5) on that face.
        CHECK_NEAR(r.hit.u, 0.5, 1e-4, "local u is the X fraction of the hit");
        CHECK_NEAR(r.hit.v, 0.5, 1e-4, "local v is the Y fraction of the hit");
    }
}

// Reach is a property of the ray, not of how many cells the eye happens to
// start in. REACH_DIST is 5.0, and the DDA tests each cell at the point the ray
// ENTERS it, so a cell is a candidate exactly when its entry point is within
// the limit. From an eye at 0.5 aimed +X, cell d is entered at distance
// d - 0.5: cells 1..5 enter at 0.5..4.5 and are all reachable, cell 6 enters at
// 5.5 and is not. That boundary is pinned in …does_not_reach_past_the_limit.
static void test_Raycaster_reaches_every_distance_up_to_the_limit() {
    for (int d = 1; d <= 5; ++d) {
        MapWorld w;
        w.set({d, 0, 0}, kStone);
        Raycaster rc(&w);
        const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f),
                            Raycaster::REACH_DIST);
        CHECK(isHit(r.targeted), "GetTargetedBlock finds the target");
        CHECK(isHit(r.hit.pos),
              "gp-nm51: RaycastHit finds a target at every distance in reach");
        if (isHit(r.hit.pos)) CHECK_EQ_I(r.hit.pos.x, d, "at the right distance");
    }

    for (int d = 1; d <= 5; ++d) {
        MapWorld w;
        w.set({0, 0, d}, kStone);
        Raycaster rc(&w);
        const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f),
                            Raycaster::REACH_DIST);
        CHECK(isHit(r.hit.pos), "gp-nm51: RaycastHit reaches down +Z too");
        if (isHit(r.hit.pos)) CHECK_EQ_I(r.hit.pos.z, d, "at the right distance");
    }

    for (int d = 1; d <= 5; ++d) {
        MapWorld w;
        w.set({0, d, 0}, kStone);
        Raycaster rc(&w);
        const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f),
                            Raycaster::REACH_DIST);
        CHECK(isHit(r.hit.pos), "gp-nm51: RaycastHit reaches up +Y too");
        if (isHit(r.hit.pos)) CHECK_EQ_I(r.hit.pos.y, d, "at the right distance");
    }
}

// A target whose cell ENTRY lies beyond the reach limit must NOT be found, on
// either raycaster. Without this, "fix the DDA" and "make the loop unbounded"
// look identical.
static void test_Raycaster_does_not_reach_past_the_limit() {
    // From an eye at x = 0.5, cell 6 is entered at distance 5.5, on a limit of
    // 5.0. (Cell 5 is entered at 4.5 and IS reachable — see the test above.)
    MapWorld w;
    w.set({6, 0, 0}, kStone);
    Raycaster rc(&w);
    const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f),
                        Raycaster::REACH_DIST);
    CHECK(!isHit(r.targeted), "GetTargetedBlock respects the reach limit");
    CHECK(!isHit(r.hit.pos),
          "gp-nm51: the fixed DDA must still stop at maxDist — a longer walk "
          "is only correct if the limit is still enforced");
}

// ---------------------------------------------------------------------------
// CONSISTENCY — the invariant that would have caught gp-nm51 immediately
// ---------------------------------------------------------------------------

// GetTargetedBlock and RaycastHit are two copies of one DDA in one file. Feed
// both the same world and the same ray and require the same answer. This is
// deliberately a cross-check instead of a second copy of the expected value:
// it fails if EITHER implementation drifts, and it cannot be satisfied by
// both drifting together.
static void test_Raycaster_the_two_entry_points_agree() {
    // A wall with a gap, so the chosen cell depends on the ray rather than on
    // there being exactly one candidate.
    MapWorld w;
    w.fill(-1, 8, -1, 1, 3, 3, kStone);  // a wall at z = 3
    w.set({2, 0, 3}, 0);                 // a one-cell doorway at (2, 0, 3)
    w.set({5, 0, 6}, kStone);            // something behind the wall
    Raycaster rc(&w);

    struct Case { float dx, dy, dz; const char *what; };
    const Case cases[] = {
        {0.0f, 0.0f, -1.0f, "straight -Z"},
        {0.0f, 0.0f, 1.0f, "straight +Z"},
        {1.0f, 0.0f, 0.0f, "straight +X"},
        {-1.0f, 0.0f, 0.0f, "straight -X"},
        {0.0f, 1.0f, 0.0f, "straight +Y"},
        {0.0f, -1.0f, 0.0f, "straight -Y"},
        {0.7071f, 0.0f, 0.7071f, "diagonal +X+Z"},
        {0.0f, 0.7071f, 0.7071f, "diagonal +Y+Z"},
        {-0.5774f, 0.5774f, -0.5774f, "three-axis diagonal"},
    };

    for (const Case &c : cases) {
        const Ray ray = rayFrom(0.5f, 0.5f, 0.5f, c.dx, c.dy, c.dz);
        const Both r = cast(rc, ray, Raycaster::REACH_DIST);
        const bool agree = isHit(r.targeted) == isHit(r.hit.pos) &&
                           (!isHit(r.targeted) ||
                            (r.targeted.x == r.hit.pos.x &&
                             r.targeted.y == r.hit.pos.y &&
                             r.targeted.z == r.hit.pos.z));
        CHECK(agree,
              "gp-nm51: both raycasters must hit the SAME cell for the same ray "
              "and world — they are two copies of one DDA, so a divergence is a "
              "bug in one of them");
    }
}

// The doorway is a real behaviour check rather than a consistency check: a ray
// aimed through the gap must reach the block BEHIND the wall, which needs the
// DDA to walk several cells in a row without a hit before the last one. Kept
// inside the 5-block reach: the eye is at z = 3.5 and the target at z = 0, so
// the ray crosses three empty cells of the doorway and lands 3.5 away.
static void test_Raycaster_walks_through_a_gap_to_the_block_behind() {
    MapWorld w;
    w.fill(-1, 8, -1, 1, 4, 4, kStone);  // wall at z = 4, all x in [-1, 8]
    w.set({0, 0, 4}, 0);                 // doorway at (0, 0, 4)
    w.set({0, 0, 0}, kStone);            // target behind the wall
    Raycaster rc(&w);

    const Both r = cast(rc, rayFrom(0.5f, 0.5f, 3.5f, 0.0f, 0.0f, -1.0f),
                        Raycaster::REACH_DIST);
    CHECK(isHit(r.targeted), "GetTargetedBlock sees through the doorway");
    CHECK(isHit(r.hit.pos),
          "gp-nm51: RaycastHit sees through a doorway to a target 3.5 cells "
          "away — it has to walk the empty cells in between without a hit");
    if (isHit(r.hit.pos)) {
        CHECK_EQ_I(r.hit.pos.z, 0, "and it is the block behind the wall");
        CHECK_EQ_I(r.hit.faceZ, 1, "entered through its +Z face");
    }
}

// ---------------------------------------------------------------------------
// The immediate-cell and boundary cases, which must keep working
// ---------------------------------------------------------------------------

// The one case the defective DDA got right, pinned so a fix cannot regress it.
static void test_Raycaster_still_finds_the_adjacent_cell() {
    MapWorld w;
    w.set({1, 0, 0}, kStone);
    Raycaster rc(&w);
    const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f),
                        Raycaster::REACH_DIST);
    CHECK(isHit(r.hit.pos), "the adjacent cell is still found");
    if (isHit(r.hit.pos)) {
        CHECK_EQ_I(r.hit.pos.x, 1, "at distance one");
        CHECK_EQ_I(r.hit.faceX, -1, "entered through -X");
    }
}

// The block the eye is INSIDE is reported, with a zero face normal — that is
// what GetPlacementPos documents as "ray started inside the block".
static void test_Raycaster_reports_a_block_the_eye_is_inside() {
    MapWorld w;
    w.set({0, 0, 0}, kStone);
    Raycaster rc(&w);
    const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f),
                        Raycaster::REACH_DIST);
    CHECK(isHit(r.hit.pos), "a ray starting inside a solid block hits it");
    if (isHit(r.hit.pos)) {
        CHECK_EQ_I(r.hit.pos.x, 0, "that block");
        CHECK_EQ_I(r.hit.faceX, 0, "with a zero face normal (no side was entered)");
    }
}

// A near-miss ray must report no hit. If the DDA ever steps out of its cell,
// or if the distance guard is dropped, this is the test that notices.
static void test_Raycaster_a_miss_reports_the_sentinel() {
    MapWorld w;
    w.set({7, 7, 7}, kStone);  // far off the ray's path
    Raycaster rc(&w);
    const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f),
                        Raycaster::REACH_DIST);
    CHECK(!isHit(r.targeted), "GetTargetedBlock reports a miss");
    CHECK(!isHit(r.hit.pos), "RaycastHit reports a miss on the same ray");
    if (!isHit(r.hit.pos)) {
        CHECK_EQ_I(r.hit.faceX, 0, "a miss carries no face");
        CHECK_EQ_I(r.hit.faceY, 0, "a miss carries no face");
        CHECK_EQ_I(r.hit.faceZ, 0, "a miss carries no face");
    }
}

// The nearest of several blocks along the ray wins, at range. A DDA that walks
// the cells out of order, or that reuses a stale point, changes this answer.
static void test_Raycaster_hits_the_nearest_block_first() {
    MapWorld w;
    for (int d = 1; d <= 5; ++d) w.set({d, 0, 0}, kStone);
    Raycaster rc(&w);
    const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f),
                        Raycaster::REACH_DIST);
    CHECK(isHit(r.hit.pos), "a wall of blocks is hit");
    if (isHit(r.hit.pos)) {
        CHECK_EQ_I(r.hit.pos.x, 1,
                   "gp-nm51: the NEAREST cell wins, so the DDA is walking the "
                   "cells in ray order and stopping at the first");
    }
}

// A zero-length direction is a degenerate input that used to be special-cased
// in both entry points. Keep that behaviour pinned.
static void test_Raycaster_a_degenerate_ray_reports_the_sentinel() {
    MapWorld w;
    w.fill(-2, 2, -2, 2, -2, 2, kStone);  // solid everywhere nearby
    Raycaster rc(&w);
    const Both r = cast(rc, rayFrom(0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f),
                        Raycaster::REACH_DIST);
    CHECK(!isHit(r.targeted), "a zero direction is not a ray");
    CHECK(!isHit(r.hit.pos), "and hits nothing, rather than looping forever");
}

}  // namespace

int main() {
    TEST(Raycaster_finds_a_target_four_cells_down_the_ray);
    TEST(Raycaster_reaches_every_distance_up_to_the_limit);
    TEST(Raycaster_does_not_reach_past_the_limit);
    TEST(Raycaster_the_two_entry_points_agree);
    TEST(Raycaster_walks_through_a_gap_to_the_block_behind);
    TEST(Raycaster_still_finds_the_adjacent_cell);
    TEST(Raycaster_reports_a_block_the_eye_is_inside);
    TEST(Raycaster_a_miss_reports_the_sentinel);
    TEST(Raycaster_hits_the_nearest_block_first);
    TEST(Raycaster_a_degenerate_ray_reports_the_sentinel);

    printf("\n=== Raycaster: %d tests, %d passed, %d failed ===\n", g_tests,
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
