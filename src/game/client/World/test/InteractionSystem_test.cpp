// Unit tests for the client-side interaction gate and the player body
// controller (issue gp-05vj).
//
//   File under test: src/game/client/World/InteractionSystem.cpp
//   File under test: src/game/client/Player/PlayerController.cpp
//
// No display, no GPU, no window, no input device and no wall-clock waiting is
// involved: the Camera is driven purely through its public `pos` (its
// orientation stays identity, so GetForward() is (0,0,-1)), InputState is a
// plain struct, and the "network" is a default-constructed NetClient whose
// Send*() methods early-return while no control connection is up. Everything
// asserted below is therefore a deterministic, pure-function observation.
//
// What is OBSERVABLE through the public API:
//
//   InteractionSystem
//     - highlight state machine  (HasHighlight / GetHighlightedBlock)
//     - held-item read           (GetHeldItem)
//     - ray-cast predicates      (RaycastTarget / RaycastTargetAtMouse /
//                                  RaycastHitAtMouse / RaycastHitAtCenter)
//     - target face              (TargetFace)
//     - the left-click gate      (World::IsBlockActionPending /
//                                  MarkBlockActionSent / ClearBlockActionPending
//                                  are real World state, so the debounce the
//                                  gate drives is directly observable)
//
//   PlayerController
//     - position / IsOnGround across fly, walk, jump, sneak and collision
//     - the flight-mode fallback when no world is attached
//
// What is NOT observable, and why (reported as a testability finding, not
// papered over): neither SendBlockAction nor SendToolAction is virtual and
// both early-return on `if (!ctrl_conn_ || !connected_ctrl_)`, and neither
// leaves any other state behind, so the decision to SEND is only observable
// through a real socket connection. The tests below therefore pin the gate's
// preconditions and its state side effects, not the wire write.
//
// Geometric note: the ±face assertions use single whole-chunk slabs (one
// plane each) instead of hand-placed blocks. A ray of any direction entering
// a slab through that slab's axis always has that axis as its last DDA step, so
// the reported face is independent of the camera pitch/yaw, the aspect ratio
// and the exact slab-crossing coordinates — only the reach distance matters.

// ---- project test harness (mirrors src/game/world/test/BlockTransforms_test.cpp) ----
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "",
           msg ? msg : "");
  }
}

#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)
#define CHECK_GE(a, b, ...) test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, ##__VA_ARGS__)

#define TEST(name) do { printf("  TEST: %s\n", #name); test_##name(); } while (0)

// Exact float comparison is fragile for the accumulated physics integration,
// so physics magnitudes go through a tolerance. Everything that is a bit-exact
// assignment (a snapped ground height, a reset velocity) is checked with
// CHECK_EQ instead.
static bool nearly(float a, float b, float eps = 1e-4f) {
  return std::fabs(a - b) <= eps;
}
#define CHECK_NEAR(a, b, ...) test_check(nearly((a), (b)), __FILE__, __LINE__, #a " ~= " #b, ##__VA_ARGS__)

#include <apps/game_client/Camera/Camera.h>
#include <apps/game_client/Common/InputState.h>
#include <apps/game_client/Network/NetClient.h>
#include <apps/game_client/World/ChunkView.h>
#include <data/registry/ToolIds.h>
#include <game/client/Player/PlayerController.h>
#include <game/client/World/InteractionSystem.h>
#include <game/client/World/WrenchingSide.h>
#include <game/client/World/World.h>
#include <game/ui/client/core/InputBinder.h>

#include <GLFW/glfw3.h>
#include <Storage/cache/MutableChunk.h>
#include <glm/glm.hpp>

namespace {

constexpr int kNoHit = std::numeric_limits<int32_t>::max();
// PlayerController's private constants, mirrored from
// src/game/client/Player/PlayerController.h. They are `private`, so the test
// restates them; every expectation below is written in the same expression
// shape the implementation uses, so the results are bit-identical.
constexpr float kSpeed = 14.317f;
constexpr float kGravity = 25.0f;
constexpr float kJumpVelocity = 8.5f;
constexpr float kEyeHeight = 1.6f;

// The chunk the fixtures live in. All fixture blocks stay inside
// x,z in [0,32) and y in [64,96) so a single chunk holds them all.
constexpr int32_t kChunkX = 0;
constexpr int32_t kChunkY = 2;
constexpr int32_t kChunkZ = 0;
constexpr uint16_t kSolid = 0x1234;

ChunkCoord fixtureCoord() { return ChunkCoord{kChunkX, kChunkY, kChunkZ}; }

// MutableChunk::setBlock takes CHUNK-LOCAL coordinates (0..31 per axis) and
// has no bounds check, so every test spells its fixture in WORLD coordinates
// and this helper translates. Out-of-range input is rejected loudly rather
// than corrupting the neighbouring section.
bool setBlock(MutableChunk &mc, int32_t x, int32_t y, int32_t z, uint16_t id) {
  const int32_t lx = x - kChunkX * 32;
  const int32_t ly = y - kChunkY * 32;
  const int32_t lz = z - kChunkZ * 32;
  if (lx < 0 || lx > 31 || ly < 0 || ly > 31 || lz < 0 || lz > 31) {
    printf("  FIXTURE ERROR: block (%d,%d,%d) outside the fixture chunk\n", x, y, z);
    ++g_failed;
    return false;
  }
  mc.setBlock(lx, ly, lz, id);
  return true;
}

// Encodes a filled MutableChunk into the wire buffer ChunkView decodes.
std::shared_ptr<ChunkView> makeChunkView(const std::function<void(MutableChunk &)> &fill) {
  MutableChunk mc;
  fill(mc);
  auto wire = std::make_shared<std::vector<uint8_t>>();
  mc.encodeToWire(*wire);
  return std::make_shared<ChunkView>(wire);
}

// A World holding exactly one chunk, shaped by `fill` (chunk-local 0..31).
struct Fixture {
  World world;

  explicit Fixture(const std::function<void(MutableChunk &)> &fill) {
    world.OnChunkData(makeChunkView(fill), fixtureCoord());
  }

  // Loads an already-encoded chunk (used for the all-air fixture).
  explicit Fixture(std::shared_ptr<ChunkView> chunk) {
    world.OnChunkData(std::move(chunk), fixtureCoord());
  }
};

// Writes a block into an ALREADY-LOADED world, bypassing the fixture builder.
// Used to mutate a live World mid-test exactly the way a server-driven
// OnBlockUpdate would.
void setBlockNoWorld(World &world, int32_t x, int32_t y, int32_t z, uint16_t id) {
  const int32_t lx = x - kChunkX * 32;
  const int32_t ly = y - kChunkY * 32;
  const int32_t lz = z - kChunkZ * 32;
  if (lx < 0 || lx > 31 || ly < 0 || ly > 31 || lz < 0 || lz > 31) {
    printf("  FIXTURE ERROR: block (%d,%d,%d) outside the fixture chunk\n", x, y, z);
    ++g_failed;
    return;
  }
  world.OnBlockUpdate(BlockPos{x, y, z}, id, 0, 0);
}

// Fills the whole y = `y` plane of the fixture chunk, in WORLD coordinates.
void fillPlane(MutableChunk &mc, int32_t y) {
  for (int32_t z = 0; z < 32; ++z)
    for (int32_t x = 0; x < 32; ++x)
      setBlock(mc, kChunkX * 32 + x, y, kChunkZ * 32 + z, kSolid);
}

// Fills the whole x = `x` plane, in WORLD coordinates.
void fillPlaneX(MutableChunk &mc, int32_t x) {
  for (int32_t z = 0; z < 32; ++z)
    for (int32_t y = 0; y < 32; ++y)
      setBlock(mc, x, kChunkY * 32 + y, kChunkZ * 32 + z, kSolid);
}

// Fills the whole z = `z` plane, in WORLD coordinates.
void fillPlaneZ(MutableChunk &mc, int32_t z) {
  for (int32_t y = 0; y < 32; ++y)
    for (int32_t x = 0; x < 32; ++x)
      setBlock(mc, kChunkX * 32 + x, kChunkY * 32 + y, z, kSolid);
}

// Empty chunk (loaded, but all air) — GetBlockAt returns 0 everywhere.
std::shared_ptr<ChunkView> makeEmptyChunkView() {
  return makeChunkView([](MutableChunk &) {});
}

// Camera with identity orientation (forward = (0,0,-1)) at a fixed eye point.
// `fwd - pos` therefore points down -Z and hits whatever is at smaller z.
Camera makeCamera(const glm::vec3 &pos) {
  Camera cam;  // orient defaults to identity — no Init(), no GLFW window
  cam.pos = pos;
  return cam;
}

constexpr float kViewportW = 800.0f;
constexpr float kViewportH = 600.0f;

// Eye position shared by the face/ray tests: inside cell (8,70,8).
const glm::vec3 kEye{8.5f, 70.5f, 8.5f};

PlayerMove makeMove(float forward = 0.0f, float right = 0.0f, bool jump = false,
                    bool sneak = false, float vertical = 0.0f) {
  PlayerMove m;
  m.forward = forward;
  m.right = right;
  m.jump = jump;
  m.sneak = sneak;
  m.vertical = vertical;
  return m;
}

InventoryState makeInventory(std::initializer_list<uint16_t> items, int selected) {
  InventoryState inv;
  inv.slots.resize(items.size());
  size_t i = 0;
  for (uint16_t id : items) {
    inv.slots[i].item_id = id;
    inv.slots[i].count = 1;
    ++i;
  }
  inv.selectedSlot = selected;
  inv.player_id = 4242;
  return inv;
}

// ===========================================================================
// InteractionSystem — held item
// ===========================================================================

static void test_HeldItemIsZeroWithoutInventory() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);

  // The default-constructed system has no InventoryState attached.
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 0);
}

static void test_HeldItemFollowsSelectedSlot() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);

  InventoryState inv = makeInventory({11, 22, 33}, 0);
  sys.SetInventory(&inv);
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 11);

  inv.selectedSlot = 2;
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 33);

  // selectedSlot == -1 is the "nothing selected" sentinel.
  inv.selectedSlot = -1;
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 0);

  // Out of range (>= slots.size()) is rejected rather than clamped.
  inv.selectedSlot = 3;
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 0);
}

static void test_HeldItemOfAnEmptyInventoryIsZero() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);

  InventoryState inv;  // no slots at all, selectedSlot defaults to -1
  sys.SetInventory(&inv);
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 0);

  inv.selectedSlot = 0;  // in range for a grown vector, still empty stack
  inv.slots.resize(1);
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 0);
}

static void test_SetInventoryCanClearThePointer() {
  Fixture fixture(makeEmptyChunkView());
  InventoryState inv = makeInventory({7}, 0);
  InteractionSystem sys(&fixture.world, &inv);
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 7);

  sys.SetInventory(nullptr);
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 0);
}

// ===========================================================================
// InteractionSystem — highlight state machine
// ===========================================================================

static void test_FreshSystemHasNoHighlight() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);

  // Nothing has been ray-cast yet: the highlight flag is false and the
  // position member is still its default-constructed zero.
  CHECK(!sys.HasHighlight());
  CHECK_EQ(sys.GetHighlightedBlock().x, 0);
  CHECK_EQ(sys.GetHighlightedBlock().y, 0);
  CHECK_EQ(sys.GetHighlightedBlock().z, 0);
}

static void test_UpdateSetsHighlightFromTheCrosshairRay() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  sys.Update(cam, InputState{}, fixture.world, net);

  CHECK(sys.HasHighlight());
  CHECK_EQ(sys.GetHighlightedBlock().x, 8);
  CHECK_EQ(sys.GetHighlightedBlock().y, 70);
  CHECK_EQ(sys.GetHighlightedBlock().z, 4);
}

static void test_UpdateWithNoTargetLeavesTheHighlightOff() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  sys.Update(cam, InputState{}, fixture.world, net);

  CHECK(!sys.HasHighlight());
}

static void test_UpdateKeepsTheLastBlockAfterLosingTheTarget() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  sys.Update(cam, InputState{}, fixture.world, net);
  CHECK(sys.HasHighlight());

  // Remove the block behind the world's back (a server-driven update).
  fixture.world.OnBlockUpdate(BlockPos{8, 70, 4}, 0, 0, 0);

  sys.Update(cam, InputState{}, fixture.world, net);
  // OBSERVED BEHAVIOUR: the flag clears, but highlightedBlock_ is only
  // assigned when a target exists, so the stale coordinate stays readable.
  CHECK(!sys.HasHighlight());
  CHECK_EQ(sys.GetHighlightedBlock().z, 4);
  // The gate itself is driven by HasHighlight(), so a stale coordinate can
  // never be broken: see the left-click tests below.
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

static void test_HighlightFollowsTheBlockTheCrosshairMovesTo() {
  Fixture fixture([](MutableChunk &mc) {
    setBlock(mc, 8, 70, 4, kSolid);
    setBlock(mc, 9, 70, 4, kSolid);
  });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  sys.Update(cam, InputState{}, fixture.world, net);
  CHECK_EQ(sys.GetHighlightedBlock().x, 8);

  // Nudge the eye to +X: the DDA now enters cell 9 first.
  Camera moved = makeCamera(glm::vec3(9.5f, 70.5f, 8.5f));
  sys.Update(moved, InputState{}, fixture.world, net);
  CHECK_EQ(sys.GetHighlightedBlock().x, 9);
}

// ===========================================================================
// InteractionSystem — ray casting
// ===========================================================================

static void test_RaycastTargetMissesOnAnEmptyWorld() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  const BlockPos hit = sys.RaycastTarget(cam);
  // A miss is the all-INT32_MAX sentinel that Raycaster returns; the caller
  // detects it by testing target.x against that sentinel.
  CHECK_EQ(hit.x, kNoHit);
  CHECK_EQ(hit.y, kNoHit);
  CHECK_EQ(hit.z, kNoHit);
}

static void test_RaycastTargetFindsABlockWithinReach() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  const BlockPos hit = sys.RaycastTarget(cam);
  CHECK_EQ(hit.x, 8);
  CHECK_EQ(hit.y, 70);
  CHECK_EQ(hit.z, 4);
}

// Reach is renderlib::Raycaster::REACH_DIST = 5.0 blocks. The DDA breaks once
// the ray point is past the limit, so the reachable cells from z = 8.5 are
// z = 7 (0.5 away) through z = 3 (5.5 away) — the block is still reported
// because the cell is entered at z = 3.0, exactly 5.5... observed: z = 3 IS
// picked and z = 2 (6.5 away) is NOT. The boundary is one cell past the
// nominal reach, which is the documented half-open behaviour of this DDA.
static void test_ReachStopsOneCellPastFiveBlocks() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 3, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // z = 3 is 5.5 blocks from the eye and is still reported.
  CHECK_EQ(sys.RaycastTarget(cam).z, 3);

  // z = 2 is 6.5 blocks away and is out of reach.
  fixture.world.OnBlockUpdate(BlockPos{8, 70, 3}, 0, 0, 0);
  setBlockNoWorld(fixture.world, 8, 70, 2, kSolid);
  CHECK_EQ(sys.RaycastTarget(cam).z, kNoHit);
}

// The nearer of two blocks in reach wins, and removing it reveals the next.
static void test_TheNearerBlockWinsAndRemovalRevealsTheNext() {
  Fixture fixture([](MutableChunk &mc) {
    setBlock(mc, 8, 70, 4, kSolid);
    setBlock(mc, 8, 70, 3, kSolid);
  });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  CHECK_EQ(sys.RaycastTarget(cam).z, 4);
  fixture.world.OnBlockUpdate(BlockPos{8, 70, 4}, 0, 0, 0);
  CHECK_EQ(sys.RaycastTarget(cam).z, 3);
}

static void test_InsideASolidBlockTheRayHitsItself() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 8, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // The DDA tests the origin cell before stepping, so a camera embedded in a
  // block targets that block with a zero face normal.
  const BlockPos hit = sys.RaycastTarget(cam);
  CHECK_EQ(hit.x, 8);
  CHECK_EQ(hit.y, 70);
  CHECK_EQ(hit.z, 8);
  CHECK_EQ(static_cast<int>(sys.TargetFace(cam)), 0);
}

static void test_MouseRayAgreesWithTheCrosshairRayAtScreenCenter() {
  Fixture fixture([](MutableChunk &mc) {
    setBlock(mc, 8, 70, 4, kSolid);
    setBlock(mc, 3, 66, 8, kSolid);  // off to the side, reachable only by a
  });                                // non-centred mouse ray
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // The exact screen centre must un-project onto the camera forward ray, so
  // the mouse-pixel picker and the crosshair picker agree. (Only
  // GetTargetedBlock is compared here; the HitInfo path's agreement with it is
  // asserted separately in test_RaycastHitAgreesWithRaycastTarget.)
  CHECK_EQ(sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, kViewportW * 0.5,
                                    kViewportH * 0.5)
               .z,
           sys.RaycastTarget(cam).z);
}

static void test_MouseRayAtAnEdgeLooksDownNotForward() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // Bottom edge of the viewport = looking down. The forward block at z = 4 is
  // not on that ray, so the hit is a miss.
  CHECK_EQ(sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, kViewportW * 0.5,
                                    kViewportH)
               .z,
           kNoHit);

  // Same for the top edge (looking up) and the left edge.
  CHECK_EQ(sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, kViewportW * 0.5, 0.0)
               .z,
           kNoHit);
  CHECK_EQ(sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, 0.0, kViewportH * 0.5)
               .z,
           kNoHit);
}

static void test_OffCenterMouseRayHitsABlockTheCrosshairMisses() {
  Fixture fixture([](MutableChunk &mc) {
    // One step of the 3x3x3 pocket around the eye stays air, and a block sits
    // diagonally forward-left, out of the straight-ahead ray.
    setBlock(mc, 6, 70, 6, kSolid);
  });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  CHECK_EQ(sys.RaycastTarget(cam).x, kNoHit);

  // The LEFT viewport edge looks down the -X/-Z diagonal and does reach it.
  const BlockPos hit =
      sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, 0.0, kViewportH * 0.5);
  CHECK_NE(hit.x, kNoHit);
  CHECK_EQ(hit.x, 6);
  CHECK_EQ(hit.y, 70);
  CHECK_EQ(hit.z, 6);

  // The RIGHT edge looks the other way and misses, and so do the vertical
  // edges — the block is 2 blocks off the centre line in X and Z.
  CHECK_EQ(sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, kViewportW,
                                    kViewportH * 0.5)
               .x,
           kNoHit);
  CHECK_EQ(sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, kViewportW * 0.5, 0.0)
               .x,
           kNoHit);
  CHECK_EQ(sys.RaycastTargetAtMouse(cam, kViewportW, kViewportH, kViewportW * 0.5,
                                    kViewportH)
               .x,
           kNoHit);
}

// PRODUCTION DEFECT — Raycaster::RaycastHit cannot find anything beyond the
// immediately adjacent cell, and InteractionSystem's two HitInfo entry points
// are built entirely on it, so they inherit the defect.
//
// The two raycasters in the same file advance the DDA point differently:
//   GetTargetedBlock (correct):  pz = ray.origin.z + tMaxZ * dz   (absolute)
//   RaycastHit      (defective): pz += tMaxZ * dz                 (relative)
// tMaxZ already grows by tDeltaZ every step, so accumulating it compounds:
// after n steps the point is off by n(n-1)/2 * tDeltaZ * dz. The drift
// inflates the measured distSq, so the `distSq > maxDistSq` guard trips
// before the DDA can reach a block that is genuinely inside REACH_DIST.
// For a straight -Z ray from z=8.5 the point reads 8.0, 6.5, 4.0, 0.5 where
// it should read 8.0, 7.0, 6.0, 5.0.
//
// Impact: RaycastHitAtMouse / RaycastHitAtCenter return the no-hit sentinel
// for every block past the first cell, so the GT-style wrench side selection
// (RaycastHitAtCenter -> hit.u/v -> determineWrenchingSide) and TargetFace's
// HitInfo agree with nothing. GetTargetedBlock — which drives highlighting,
// the highlight state machine and the left-click gate — is unaffected.
//
// The tests below pin the OBSERVED behaviour. Fixing RaycastHit is out of
// scope for this issue (the file lives in src/apps/game_client/RenderLib).
static void test_RaycastHitFindsTheImmediatelyAdjacentCell() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 7, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // One step in, the relative and absolute updates coincide, so the adjacent
  // cell IS found.
  const renderlib::Raycaster::HitInfo hit =
      sys.RaycastHitAtCenter(cam, kViewportW, kViewportH);
  CHECK_EQ(hit.pos.x, 8);
  CHECK_EQ(hit.pos.y, 70);
  CHECK_EQ(hit.pos.z, 7);
  // Entered through the +Z face, and the face/u/v fields are self-consistent.
  CHECK_EQ(hit.faceZ, 1);
  CHECK_EQ(hit.faceX, 0);
  CHECK_EQ(hit.faceY, 0);
  CHECK_NEAR(hit.u, 0.5f);  // X fraction of the entered face
  CHECK_NEAR(hit.v, 0.5f);  // Z fraction of the entered face
}

static void test_RaycastHitAgreesWithRaycastTarget() {
  // The block picker used by the highlight/click gate and the HitInfo raycaster
  // behind the wrench selection MUST see the same world.
  //
  // This test used to assert the opposite - that the picker found a far block
  // "while the HitInfo raycaster for the SAME camera reports no hit at all" -
  // i.e. it characterised a bug as if it were a contract, and its name said so.
  // RaycastHit advanced its DDA point relatively (px += tMaxX*dx) while
  // GetTargetedBlock used the absolute form; tMax* is not a step length, it
  // already grows by tDelta* each iteration, so the relative form compounded
  // and the walker left the ray's line entirely. The inflated distance then
  // tripped the reach guard while the DDA was still nowhere near maxDist.
  // Fixed in Raycaster.cpp (gp-nm51).
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  const BlockPos picked = sys.RaycastTarget(cam);
  CHECK_EQ(picked.z, 4);

  // The SAME camera now finds the SAME block through the HitInfo path.
  const renderlib::Raycaster::HitInfo hit =
      sys.RaycastHitAtCenter(cam, kViewportW, kViewportH);
  CHECK_EQ(hit.pos.z, picked.z);
  CHECK_EQ(hit.pos.x, picked.x);
  CHECK_EQ(hit.pos.y, picked.y);

  // And not merely at the centre ray: an off-centre mouse pixel produces a
  // non-axis-aligned ray, which drifted even faster under the old form.
  Fixture diag([](MutableChunk &mc) { setBlock(mc, 6, 70, 6, kSolid); });
  InteractionSystem diagSys(&diag.world);
  const Camera diagCam = makeCamera(kEye);
  CHECK_EQ(diagSys.RaycastTargetAtMouse(diagCam, kViewportW, kViewportH, 0.0,
                                        kViewportH * 0.5)
               .x,
           6);
  CHECK_EQ(diagSys.RaycastHitAtMouse(diagCam, kViewportW, kViewportH, 0.0,
                                     kViewportH * 0.5)
               .pos.x,
           6);
}

// A tall, narrow viewport is a legal aspect and must un-project correctly:
// the screen centre still lands on the camera forward ray.
static void test_ExtremeAspectStillHitsFromTheScreenCentre() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 7, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // The adjacent cell is found at any sane aspect ratio, because the
  // defective relative advance has not had time to drift yet.
  const BlockPos picked =
      sys.RaycastTargetAtMouse(cam, 4000.0f, 1.0f, 2000.0, 0.5);
  CHECK_EQ(picked.z, 7);
  CHECK_EQ(sys.RaycastHitAtMouse(cam, 4000.0f, 1.0f, 2000.0, 0.5).pos.z, 7);
}

// ===========================================================================
// InteractionSystem — target face (0=DOWN .. 5=EAST)
// ===========================================================================

static void test_TargetFaceOfTheForwardRayIsSouth() {
  // A single z-plane: the -Z forward ray enters it through its +Z face. Placed
  // at the ADJACENT cell z = 7 so the HitInfo raycaster can still reach it.
  Fixture fixture([](MutableChunk &mc) { fillPlaneZ(mc, 7); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  CHECK_EQ(static_cast<int>(sys.TargetFace(cam)), 3);
  // The equivalent HitInfo agrees.
  const renderlib::Raycaster::HitInfo hit =
      sys.RaycastHitAtCenter(cam, kViewportW, kViewportH);
  CHECK_EQ(hit.pos.z, 7);
  CHECK_EQ(hit.faceZ, 1);
  CHECK_EQ(hit.faceX, 0);
  CHECK_EQ(hit.faceY, 0);
  CHECK_EQ(static_cast<int>(faceNormalToWireSide(hit.faceX, hit.faceY, hit.faceZ)), 3);
}

static void test_TargetFaceOfAMissIsDown() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // No target at all: the DDA zeroes all three face outputs and the mapping
  // table falls through to its default, 0 (DOWN).
  CHECK_EQ(static_cast<int>(sys.TargetFace(cam)), 0);
}

// PRODUCTION DEFECT CONSEQUENCE: the camera sits in cell (8,70,8), and the
// only cell RaycastHit can still reach on any axis is the ADJACENT one. A
// full slab one cell away along the probed axis is therefore the smallest
// configuration that still yields a hit.
//
// The face the ray ENTERS through is the axis it last stepped along, so a
// slab at y = 71 (above) is entered through -Y and a slab at y = 69 (below)
// through +Y. Note the observed inversion: looking UP enters the block
// through its DOWN face and vice versa.
static void test_MouseRayReportsTheAdjacentCellsFourFaces() {
  {  // slab one cell above the eye, probed through the top edge
    Fixture fixture([](MutableChunk &mc) { fillPlane(mc, 71); });
    InteractionSystem sys(&fixture.world);
    const Camera cam = makeCamera(kEye);
    const renderlib::Raycaster::HitInfo hit = sys.RaycastHitAtMouse(
        cam, kViewportW, kViewportH, kViewportW * 0.5, 0.0);
    CHECK_NE(hit.pos.y, kNoHit);
    CHECK_EQ(hit.pos.y, 71);
    CHECK_EQ(hit.faceY, -1);
    CHECK_EQ(hit.faceX, 0);
    CHECK_EQ(hit.faceZ, 0);
    CHECK_EQ(static_cast<int>(faceNormalToWireSide(hit.faceX, hit.faceY, hit.faceZ)), 0);
  }
  {  // slab one cell below the eye, probed through the bottom edge
    Fixture fixture([](MutableChunk &mc) { fillPlane(mc, 69); });
    InteractionSystem sys(&fixture.world);
    const Camera cam = makeCamera(kEye);
    const renderlib::Raycaster::HitInfo hit = sys.RaycastHitAtMouse(
        cam, kViewportW, kViewportH, kViewportW * 0.5, kViewportH);
    CHECK_NE(hit.pos.y, kNoHit);
    CHECK_EQ(hit.pos.y, 69);
    CHECK_EQ(hit.faceY, 1);
    CHECK_EQ(hit.faceX, 0);
    CHECK_EQ(hit.faceZ, 0);
    CHECK_EQ(static_cast<int>(faceNormalToWireSide(hit.faceX, hit.faceY, hit.faceZ)), 1);
  }
  {  // slab one cell to the -X side, probed through the left edge
    Fixture fixture([](MutableChunk &mc) { fillPlaneX(mc, 7); });
    InteractionSystem sys(&fixture.world);
    const Camera cam = makeCamera(kEye);
    const renderlib::Raycaster::HitInfo hit = sys.RaycastHitAtMouse(
        cam, kViewportW, kViewportH, 0.0, kViewportH * 0.5);
    CHECK_NE(hit.pos.x, kNoHit);
    CHECK_EQ(hit.pos.x, 7);
    CHECK_EQ(hit.faceX, 1);
    CHECK_EQ(hit.faceY, 0);
    CHECK_EQ(hit.faceZ, 0);
    CHECK_EQ(static_cast<int>(faceNormalToWireSide(hit.faceX, hit.faceY, hit.faceZ)), 5);
  }
  {  // slab one cell to the +X side is NOT reachable: the +X ray walks
     // diagonally into the z = 7 column first. See
     // test_PositiveXFaceIsUnreachableThroughRaycastHit.
    Fixture fixture([](MutableChunk &mc) { setBlock(mc, 9, 70, 8, kSolid); });
    InteractionSystem sys(&fixture.world);
    const Camera cam = makeCamera(kEye);
    const renderlib::Raycaster::HitInfo hit = sys.RaycastHitAtMouse(
        cam, kViewportW, kViewportH, kViewportW, kViewportH * 0.5);
    CHECK_EQ(hit.pos.x, kNoHit);
  }
}

// OBSERVED LIMITATION: a lone block in the cell immediately to the +X side of
// the eye is not found by either raycaster, on either entry point. The +X ray
// is tilted (it also travels -Z), so it walks out of the (9,70,8) cell on the
// very first DDA step and the accumulated-drift guard in RaycastHit — or the
// reach guard in GetTargetedBlock — fires before the cell is ever sampled.
static void test_PositiveXFaceIsUnreachableThroughRaycastHit() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 9, 70, 8, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  CHECK_EQ(sys.RaycastTarget(cam).x, kNoHit);
  CHECK_EQ(sys.RaycastHitAtCenter(cam, kViewportW, kViewportH).pos.x, kNoHit);
  CHECK_EQ(sys.RaycastHitAtMouse(cam, kViewportW, kViewportH, kViewportW,
                                 kViewportH * 0.5)
               .pos.x,
           kNoHit);
  // A FULL -X slab at the same distance IS found, so the miss above is about
  // the cell, not about the side.
  Fixture slab([](MutableChunk &mc) { fillPlaneX(mc, 7); });
  InteractionSystem slabSys(&slab.world);
  const Camera slabCam = makeCamera(kEye);
  CHECK_EQ(slabSys.RaycastTargetAtMouse(slabCam, kViewportW, kViewportH, 0.0,
                                        kViewportH * 0.5)
               .x,
           7);
}

// TargetFace is driven by GetTargetedBlock, not RaycastHit, so it is NOT
// subject to the drift defect — the two entry points disagree on the same
// camera, which is the observable consequence.
static void test_TargetFaceDisagreesWithRaycastHitOnTheSameCamera() {
  Fixture fixture([](MutableChunk &mc) { fillPlaneZ(mc, 7); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);

  // The picker enters the z = 7 slab through its +Z face -> SOUTH (3).
  CHECK_EQ(static_cast<int>(sys.TargetFace(cam)), 3);
  const BlockPos picked = sys.RaycastTarget(cam);
  CHECK_EQ(picked.z, 7);

  // The HitInfo raycaster agrees on the POSITION here, because z = 7 is the
  // adjacent cell. It agrees on the face too.
  const renderlib::Raycaster::HitInfo hit =
      sys.RaycastHitAtCenter(cam, kViewportW, kViewportH);
  CHECK_EQ(hit.pos.z, 7);
  CHECK_EQ(hit.faceZ, 1);
  CHECK_EQ(static_cast<int>(faceNormalToWireSide(hit.faceX, hit.faceY, hit.faceZ)), 3);
}

static void test_HitInfoUvFeedsTheWrenchGrid() {
  // Adjacent cell in front, entered through its +Z face: u is the X fraction
  // and v the Z fraction of that face.
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 7, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  const renderlib::Raycaster::HitInfo hit =
      sys.RaycastHitAtCenter(cam, kViewportW, kViewportH);

  CHECK_EQ(hit.pos.z, 7);
  CHECK_GE(hit.u, 0.0f);
  CHECK_GE(hit.v, 0.0f);
  CHECK_GE(1.0f, hit.u);
  CHECK_GE(1.0f, hit.v);

  const uint8_t side = faceNormalToWireSide(hit.faceX, hit.faceY, hit.faceZ);
  // Whatever cell the ray landed in, the wrench grid must return one of the
  // six wire faces and never the sentinel value.
  CHECK_GE(side, 0);
  CHECK(static_cast<int>(side) <= 5);
  CHECK_GE(determineWrenchingSide(side, hit.u, hit.v), 0);
  CHECK(static_cast<int>(determineWrenchingSide(side, hit.u, hit.v)) <= 5);
}

static void test_WrenchGridAgreesWithTheHitFaceInTheCentre() {
  // A dead-centre hit on a face maps back to that same face.
  CHECK_EQ(static_cast<int>(determineWrenchingSide(0, 0.5f, 0.5f)), 0);
  CHECK_EQ(static_cast<int>(determineWrenchingSide(1, 0.5f, 0.5f)), 1);
  CHECK_EQ(static_cast<int>(determineWrenchingSide(2, 0.5f, 0.5f)), 2);
  CHECK_EQ(static_cast<int>(determineWrenchingSide(3, 0.5f, 0.5f)), 3);
  CHECK_EQ(static_cast<int>(determineWrenchingSide(4, 0.5f, 0.5f)), 4);
  CHECK_EQ(static_cast<int>(determineWrenchingSide(5, 0.5f, 0.5f)), 5);
  // A corner on a DOWN face toggles the opposite (UP) face.
  CHECK_EQ(static_cast<int>(determineWrenchingSide(0, 0.1f, 0.1f)), 1);
}

// ===========================================================================
// InteractionSystem — the left-click gate
// ===========================================================================

static void test_LeftClickMarksTheActionPending() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  const BlockPos target{8, 70, 4};

  InputState press;
  press.mouseLeftPressed = true;
  sys.Update(cam, press, fixture.world, net);

  CHECK(sys.HasHighlight());
  // The gate fired: the position is now debounced.
  CHECK(fixture.world.IsBlockActionPending(target));
  // The debounce is per position, not global.
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 3}));
}

static void test_LeftClickIsDebouncedWhileTheActionIsPending() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  const BlockPos target{8, 70, 4};

  InputState press;
  press.mouseLeftPressed = true;
  sys.Update(cam, press, fixture.world, net);
  CHECK(fixture.world.IsBlockActionPending(target));

  // Second click while the first is in flight: the gate stays closed. The
  // pending flag remains set, and the block itself is untouched.
  sys.Update(cam, press, fixture.world, net);
  CHECK(fixture.world.IsBlockActionPending(target));
  CHECK_EQ(static_cast<int>(fixture.world.GetBlockAt(target)), static_cast<int>(kSolid));

  // Server ack clears the debounce, and the next click goes through again.
  fixture.world.ClearBlockActionPending(target);
  CHECK(!fixture.world.IsBlockActionPending(target));
  sys.Update(cam, press, fixture.world, net);
  CHECK(fixture.world.IsBlockActionPending(target));
}

static void test_LeftClickWithNoTargetIsIgnored() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  InputState press;
  press.mouseLeftPressed = true;
  sys.Update(cam, press, fixture.world, net);

  CHECK(!sys.HasHighlight());
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{0, 0, 0}));
}

static void test_HeldMouseButtonWithoutThePressEdgeDoesNotBreak() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  // mouseLeft (held) without mouseLeftPressed (the edge) must not gate open.
  InputState held;
  held.mouseLeft = true;
  sys.Update(cam, held, fixture.world, net);

  CHECK(sys.HasHighlight());
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

static void test_ClickAfterLosingTheTargetCannotBreakAStaleBlock() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  InputState press;
  press.mouseLeftPressed = true;
  sys.Update(cam, press, fixture.world, net);
  fixture.world.ClearBlockActionPending(BlockPos{8, 70, 4});

  // The block disappears; highlightedBlock_ still holds the stale coordinate.
  fixture.world.OnBlockUpdate(BlockPos{8, 70, 4}, 0, 0, 0);
  sys.Update(cam, press, fixture.world, net);
  CHECK(!sys.HasHighlight());

  // hasHighlight_ is false, so the gate stays shut despite the stale member.
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

static void test_RightClickIsNotABreakGesture() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  InputState press;
  press.mouseRightPressed = true;
  sys.Update(cam, press, fixture.world, net);

  CHECK(sys.HasHighlight());
  // InteractionSystem only breaks on left-click; placement lives in GameClient.
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

// ===========================================================================
// InteractionSystem — the wrench-cycle gate
// ===========================================================================

// The wrench branch's only side effect would be the SendToolAction write, which
// a disconnected NetClient drops. What IS pinned here is that the branch's
// preconditions are read as advertised and that the branch never disturbs the
// left-click debounce state.
static void test_WrenchGateRunsOnlyWithAHighlight() {
  Fixture fixture(makeEmptyChunkView());
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  InputBinder binder;  // no window, no GLFW init — only GLFW key constants
  sys.SetBinder(&binder);
  CHECK_EQ(binder.GetHeldKey("wrench_cycle"), GLFW_KEY_G);

  InputState gHeld;
  gHeld.keys[GLFW_KEY_G] = true;
  CHECK(binder.IsHeld("wrench_cycle", gHeld));

  sys.Update(cam, gHeld, fixture.world, net);
  // No target -> the wrench branch's `&& hasHighlight_` is false.
  CHECK(!sys.HasHighlight());
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{0, 0, 0}));
}

static void test_WrenchGateWithWrenchHeldLeavesTheDebounceAlone() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  InputBinder binder;
  sys.SetBinder(&binder);

  InventoryState inv = makeInventory({ITEM_WRENCH}, 0);
  sys.SetInventory(&inv);
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), static_cast<int>(ITEM_WRENCH));

  InputState gHeld;
  gHeld.keys[GLFW_KEY_G] = true;

  // No left-click on this frame, so nothing must be marked pending even
  // though every wrench precondition (highlight + G held + wrench held) is met.
  sys.Update(cam, gHeld, fixture.world, net);
  CHECK(sys.HasHighlight());
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

static void test_WrenchGateIsSkippedForANonWrenchItem() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  InputBinder binder;
  sys.SetBinder(&binder);

  InventoryState inv = makeInventory({ITEM_DRILL_HV}, 0);
  sys.SetInventory(&inv);
  CHECK_NE(static_cast<int>(sys.GetHeldItem()), static_cast<int>(ITEM_WRENCH));

  InputState gHeld;
  gHeld.keys[GLFW_KEY_G] = true;
  sys.Update(cam, gHeld, fixture.world, net);
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

static void test_NoBinderMeansNoWrenchQuery() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;

  InputState gHeld;
  gHeld.keys[GLFW_KEY_G] = true;
  // binder_ is null: the `binder_ &&` short-circuit must keep this safe.
  sys.Update(cam, gHeld, fixture.world, net);
  CHECK(sys.HasHighlight());
  CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

// The permission matrix, asserted through the SYSTEM rather than through the
// caller. GameClient::Update already refuses to call Update() unless
// CanInteractWithWorld, so nothing in the shipped client reaches the break
// path in SPECTATOR; this pins that the refusal does not depend on the caller
// having remembered to gate it (beads gp-n86q). Previously this test asserted
// the OPPOSITE — that the break still went out — and passed, pinning the
// defect green.
static void test_SpectatorLeftClickIsRefusedInsideInteractionSystem() {
  // The matrix itself first, so a failure below is unambiguously the gate and
  // not a changed permission table.
  CHECK(!GameModePerm::CanBreak(GameMode::SPECTATOR));
  CHECK(!GameModePerm::CanBreak(GameMode::ADVENTURE));
  CHECK(GameModePerm::CanBreak(GameMode::SURVIVAL));
  CHECK(GameModePerm::CanBreak(GameMode::CREATIVE));

  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  const BlockPos target{8, 70, 4};

  InventoryState inv = makeInventory({ITEM_WRENCH}, 0);
  inv.gameMode = GameMode::SPECTATOR;  // CanBreak == false
  sys.SetInventory(&inv);

  InputState press;
  press.mouseLeftPressed = true;
  sys.Update(cam, press, fixture.world, net);

  // The refusal is scoped to the mutation, not to the ray-cast: a spectator
  // still sees the highlight (the HUD renders it), it just never debounces a
  // break into the world.
  CHECK(sys.HasHighlight());
  CHECK_EQ(static_cast<int>(sys.GetHighlightedBlock().x), 8);
  // THE ASSERTION: no world mutation leaves a mode that may not mutate one.
  CHECK(!fixture.world.IsBlockActionPending(target));

  // Positive control, same fixture and same input, one permission flip away.
  // Without this the test would also pass if the gate denied EVERY mode.
  fixture.world.ClearBlockActionPending(target);
  inv.gameMode = GameMode::SURVIVAL;
  sys.Update(cam, press, fixture.world, net);
  CHECK(fixture.world.IsBlockActionPending(target));
}

static void test_AdventureLeftClickIsRefusedInsideInteractionSystem() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  const BlockPos target{8, 70, 4};

  InventoryState inv = makeInventory({ITEM_WRENCH}, 0);
  inv.gameMode = GameMode::ADVENTURE;  // CanBreak == false
  sys.SetInventory(&inv);

  InputState press;
  press.mouseLeftPressed = true;
  sys.Update(cam, press, fixture.world, net);
  CHECK(!fixture.world.IsBlockActionPending(target));

  // Positive control: CREATIVE is the adjacent permitted mode.
  inv.gameMode = GameMode::CREATIVE;
  sys.Update(cam, press, fixture.world, net);
  CHECK(fixture.world.IsBlockActionPending(target));
}

// GameMode arrives off the wire as a bare, unchecked uint8, so the gate has to
// fail CLOSED on a value the enum does not name. A deny-list ("not ADVENTURE
// and not SPECTATOR") admits every such value; the allow-list the matrix is
// written as does not. This is the gp-ul16 lesson applied at the layer that
// actually emits the mutation.
static void test_UndefinedGameModeFailsClosedInsideInteractionSystem() {
  for (uint8_t raw : {uint8_t{4}, uint8_t{9}, uint8_t{200}, uint8_t{255}}) {
    CHECK(!IsDefinedGameMode(static_cast<GameMode>(raw)));
    CHECK(!GameModePerm::CanBreak(static_cast<GameMode>(raw)));
    CHECK(!GameModePerm::CanInteractWithWorld(static_cast<GameMode>(raw)));

    Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
    InteractionSystem sys(&fixture.world);
    const Camera cam = makeCamera(kEye);
    NetClient net;

    InventoryState inv = makeInventory({ITEM_WRENCH}, 0);
    inv.gameMode = static_cast<GameMode>(raw);
    sys.SetInventory(&inv);

    InputState press;
    press.mouseLeftPressed = true;
    sys.Update(cam, press, fixture.world, net);
    CHECK(!fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
  }
}

// The gate reads the mode off the inventory, which is its single owner. A
// system constructed with NO inventory has no mode to enforce, and the gate
// resolves to InventoryState's own default. Pinned so that choice is a
// decision rather than an accident of a null check — and so the
// inventory-less tests elsewhere in this file, which exercise the debounce
// rather than the permission, keep their meaning.
static void test_NoInventoryResolvesToTheDefaultMode() {
  InventoryState defaults;
  CHECK_EQ(static_cast<int>(defaults.gameMode),
           static_cast<int>(GameMode::CREATIVE));
  CHECK(GameModePerm::CanInteractWithWorld(defaults.gameMode));

  // And the consequence on the real system: with no inventory attached, the
  // resolved mode permits, so a left-click still debounces exactly as before.
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 70, 4, kSolid); });
  InteractionSystem sys(&fixture.world);
  CHECK_EQ(static_cast<int>(sys.GetHeldItem()), 0);
  const Camera cam = makeCamera(kEye);
  NetClient net;
  InputState press;
  press.mouseLeftPressed = true;
  sys.Update(cam, press, fixture.world, net);
  CHECK(fixture.world.IsBlockActionPending(BlockPos{8, 70, 4}));
}

// ===========================================================================
// PlayerController — flight
// ===========================================================================

static void test_ControllerStartsAtTheSpawnPoint() {
  PlayerController ctrl;
  CHECK_NEAR(ctrl.pos.x, 256.0f);
  CHECK_NEAR(ctrl.pos.y, 80.0f);
  CHECK_NEAR(ctrl.pos.z, 224.0f);
  CHECK(!ctrl.IsOnGround());
}

static void test_FlightMovesAlongTheLookVector() {
  PlayerController ctrl;
  ctrl.pos = {0.0f, 0.0f, 0.0f};

  const float dt = 0.1f;
  ctrl.Update(dt, makeMove(/*forward=*/1.0f), /*flight=*/true,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));

  const float step = kSpeed * dt;
  CHECK_NEAR(ctrl.pos.z, -step);
  CHECK_NEAR(ctrl.pos.x, 0.0f);
  // Flight never reports ground contact and zeroes the vertical velocity.
  CHECK(!ctrl.IsOnGround());
}

static void test_FlightUsesTheFullThreeDimensionalLookVector() {
  PlayerController ctrl;
  ctrl.pos = {0.0f, 0.0f, 0.0f};

  // Unlike the walk path, flight does NOT flatten the look vectors: looking
  // straight up while holding forward climbs.
  const float dt = 0.1f;
  ctrl.Update(dt, makeMove(/*forward=*/1.0f), /*flight=*/true,
              glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, kSpeed * dt);
  CHECK_NEAR(ctrl.pos.z, 0.0f);
}

static void test_FlightVerticalInputMovesOnItsOwn() {
  PlayerController ctrl;
  ctrl.pos = {0.0f, 0.0f, 0.0f};

  const float dt = 0.5f;
  // Descending (vertical = -1) drops; the look vectors contribute nothing.
  ctrl.Update(dt, makeMove(0.0f, 0.0f, false, false, /*vertical=*/-1.0f),
              /*flight=*/true, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, -kSpeed * dt);
}

static void test_FlightIsNeverBlockedByBlocks() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 0, 64, 0, kSolid); });
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {0.5f, 64.5f, 0.5f};

  const float dt = 0.1f;
  // Holding "right" with lookRight = +X walks the body straight into the block
  // that occupies the cell it is standing in.
  ctrl.Update(dt, makeMove(/*forward=*/0.0f, /*right=*/1.0f), /*flight=*/true,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  // Flight never consults the world: no collision response, no ground report,
  // and the position advances by exactly SPEED * dt.
  CHECK(!ctrl.IsOnGround());
  CHECK_NEAR(ctrl.pos.x, 0.5f + kSpeed * dt);
  CHECK_NEAR(ctrl.pos.y, 64.5f);
  CHECK_NEAR(ctrl.pos.z, 0.5f);
}

static void test_MissingWorldFallsBackToFlight() {
  PlayerController ctrl;
  // No SetWorld at all: walk mode has nothing to collide with, so the
  // controller degrades to the flight integrator instead of dereferencing.
  ctrl.pos = {0.0f, 0.0f, 0.0f};

  const float dt = 0.1f;
  ctrl.Update(dt, makeMove(/*forward=*/1.0f), /*flight=*/false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.z, -(kSpeed * dt));
  CHECK(!ctrl.IsOnGround());
}

static void test_ExplicitSetToNullAlsoFallsBackToFlight() {
  Fixture fixture([](MutableChunk &mc) { setBlock(mc, 8, 64, 8, kSolid); });
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.SetWorld(nullptr);
  ctrl.pos = {8.5f, 66.0f, 8.5f};

  const float dt = 0.1f;
  ctrl.Update(dt, makeMove(), /*flight=*/false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  // Falling in walk mode would drop y; the flight fallback leaves it put.
  CHECK_NEAR(ctrl.pos.y, 66.0f);
  CHECK(!ctrl.IsOnGround());
}

// ===========================================================================
// PlayerController — walk: gravity and ground contact
// ===========================================================================

// ---------------------------------------------------------------------------
// PlayerController — walk: gravity and ground contact
//
// Every expectation below was measured against the real implementation with a
// full 32x32 floor at y = 64 (the chunk is #2, so world y = 64 is chunk-local
// y = 0) and the eye resting at 66.6. Key observed facts, all of which the
// comments rely on:
//
//   * The floor top is y = 65 and the resting eye height is 65 + 1.6 = 66.6.
//   * Ground contact is reported as 65.0f + EYE_HEIGHT exactly.
//   * Gravity is applied BEFORE the ground snap, so a grounded frame moves
//     y by (-GRAVITY*dt)*dt downward and then snaps straight back: a standing
//     player never accumulates fall speed.
//   * Dropping from y = 70 takes 9 frames of dt = 0.05 to land.
// ---------------------------------------------------------------------------

// A 1-block-thick floor at y = 64 over the whole chunk.
Fixture makeFloorWorld() {
  return Fixture([](MutableChunk &mc) { fillPlane(mc, 64); });
}

static void test_WalkFallsUntilItLandsOnTheFloor() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 70.0f, 8.5f};

  const float dt = 0.05f;
  // First frame: airborne, no jump held -> gravity only.
  ctrl.Update(dt, makeMove(), /*flight=*/false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(!ctrl.IsOnGround());
  CHECK_NEAR(ctrl.pos.y, 70.0f + (-kGravity * dt) * dt);

  // Keep stepping until the controller reports contact.
  int frames = 0;
  while (!ctrl.IsOnGround() && frames < 100) {
    ctrl.Update(dt, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
                glm::vec3(1.0f, 0.0f, 0.0f));
    ++frames;
  }
  CHECK(ctrl.IsOnGround());
  // OBSERVED: 9 frames of dt = 0.05 from y = 70.
  CHECK_EQ(frames, 9);
  // Snapped exactly onto ground + EYE_HEIGHT (65 + 1.6).
  CHECK_EQ(ctrl.pos.y, 65.0f + kEyeHeight);
}

static void test_StandingStillKeepsThePlayerGrounded() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  for (int i = 0; i < 10; ++i) {
    ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
                glm::vec3(1.0f, 0.0f, 0.0f));
  }
  // Gravity is applied and immediately cancelled by the ground snap, so the
  // resting height is a fixed point.
  CHECK(ctrl.IsOnGround());
  CHECK_EQ(ctrl.pos.y, 65.0f + kEyeHeight);
}

static void test_WalkingDoesNotClimbIntoTheGround() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};
  const float z0 = ctrl.pos.z;

  const float dt = 0.05f;
  for (int i = 0; i < 8; ++i) {
    ctrl.Update(dt, makeMove(/*forward=*/1.0f), false, glm::vec3(0.0f, 0.0f, -1.0f),
                glm::vec3(1.0f, 0.0f, 0.0f));
  }
  // Horizontal motion happens (8 frames at SPEED*dt each) and y is unchanged.
  CHECK_NEAR(ctrl.pos.z - z0, -8.0f * kSpeed * dt);
  CHECK_NEAR(ctrl.pos.y, 65.0f + kEyeHeight);
  CHECK(ctrl.IsOnGround());
}

static void test_WalkingOffALedgeStartsFalling() {
  // Floor only for z <= 8; stepping to z > 8 walks off the edge.
  Fixture fixture([](MutableChunk &mc) {
    for (int z = 0; z <= 8; ++z)
      for (int x = 0; x < 32; ++x)
        setBlock(mc, x, 64, z, kSolid);
  });
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  // Settle first.
  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(ctrl.IsOnGround());
  CHECK_NEAR(ctrl.pos.z, 8.5f);

  // A right-strafe of one full frame at dt = 0.1 moves 1.4317 blocks, carrying
  // the centre from z = 8.5 to z = 9.93 — past the floor's last column, so the
  // ground scan finds nothing below and the player starts to fall.
  ctrl.Update(0.1f, makeMove(0.0f, /*right=*/1.0f), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
  CHECK_NEAR(ctrl.pos.z, 9.9317f);
  CHECK(!ctrl.IsOnGround());
  CHECK(ctrl.pos.y < 66.6f);
}

// ---------------------------------------------------------------------------
// PlayerController — jump
// ---------------------------------------------------------------------------

static void test_JumpOnlyAppliesFromTheGround() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  // Land first.
  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(ctrl.IsOnGround());

  const float dt = 0.05f;
  ctrl.Update(dt, makeMove(0.0f, 0.0f, /*jump=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  // Launched with JUMP_VELOCITY. OBSERVED: the frame rises to 67.025 rather
  // than 66.9625 — the launch happens AFTER the gravity subtraction, so the
  // frame keeps JUMP_VELOCITY for the whole step and the body gains
  // JUMP_VELOCITY * dt, not (JUMP_VELOCITY - GRAVITY*dt) * dt.
  CHECK(!ctrl.IsOnGround());
  CHECK_NEAR(ctrl.pos.y, 66.6f + kJumpVelocity * dt);
  const float frame0 = ctrl.pos.y;

  // The SECOND frame is plain gravity: the velocity is JUMP_VELOCITY minus one
  // frame of gravity, so the step is smaller than the launch step.
  ctrl.Update(dt, makeMove(0.0f, 0.0f, /*jump=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  // OBSERVED: 67.3875, i.e. +0.3625 = (8.5 - 1.25) * 0.05 — still rising.
  CHECK_NEAR(ctrl.pos.y, frame0 + (kJumpVelocity - kGravity * dt) * dt);
  // A jump held mid-air is ignored (the branch requires onGround_), so the
  // third frame is pure gravity from the accumulated velocity.
  const float frame1 = ctrl.pos.y;
  ctrl.Update(dt, makeMove(0.0f, 0.0f, /*jump=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, frame1 + (kJumpVelocity - 2.0f * kGravity * dt) * dt);
}

static void test_JumpIsSuppressedWhileSneaking() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(ctrl.IsOnGround());

  // sneak && jump -> sneaking wins, so no launch. The frame instead just
  // applies the sneak eye height, which drops the body slightly: 66.5375.
  ctrl.Update(0.05f, makeMove(0.0f, 0.0f, /*jump=*/true, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, 66.5375f);
  // The body never climbs: the following frame returns to the standing rest
  // height rather than gaining any upward velocity. (The sneak jitter itself
  // is pinned separately in test_SneakingJittersInsteadOfSettling.)
  ctrl.Update(0.05f, makeMove(0.0f, 0.0f, /*jump=*/true, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, 65.0f + kEyeHeight);
  CHECK(ctrl.pos.y < 67.0f);
}

static void test_SneakingInMidAirDoesNotSuppressTheJump() {
  // `sneaking = move.sneak && onGround_` — off the ground the sneak flag is
  // dropped, so it cannot veto anything.
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 70.0f, 8.5f};

  ctrl.Update(0.05f, makeMove(0.0f, 0.0f, /*jump=*/true, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(!ctrl.IsOnGround());
  CHECK(ctrl.pos.y > 69.9f);
}

// ---------------------------------------------------------------------------
// PlayerController — sneak edge guard
// ---------------------------------------------------------------------------

static void test_SneakStopsAtALedge() {
  // Floor up to x = 8, so the block right of x = 8.5 is empty.
  Fixture fixture([](MutableChunk &mc) {
    for (int x = 0; x <= 8; ++x)
      for (int z = 0; z < 32; ++z)
        setBlock(mc, x, 64, z, kSolid);
  });
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(ctrl.IsOnGround());
  const float x0 = ctrl.pos.x;
  CHECK_NEAR(x0, 8.5f);

  // A full-speed strafe right would step off; sneaking cancels the horizontal
  // component instead.
  ctrl.Update(0.1f, makeMove(0.0f, /*right=*/1.0f, false, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.x, x0);
}

static void test_WithoutSneakTheSameStrifeFallsOff() {
  Fixture fixture([](MutableChunk &mc) {
    for (int x = 0; x <= 8; ++x)
      for (int z = 0; z < 32; ++z)
        setBlock(mc, x, 64, z, kSolid);
  });
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(ctrl.IsOnGround());
  const float x0 = ctrl.pos.x;

  // Identical input, no sneak: the body moves to x = 9.93 and starts falling.
  ctrl.Update(0.1f, makeMove(0.0f, /*right=*/1.0f), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.x, 9.9317f);
  CHECK(ctrl.pos.x > x0);
  CHECK(!ctrl.IsOnGround());
}

// PRODUCTION OBSERVATION: the sneak eye height does not produce a stable
// resting position. With a full floor, holding sneak alternates the body
// between y = 66.5375 and y = 66.6 from frame to frame, and IsOnGround()
// toggles with it. The reason is visible in the implementation: gravity is
// applied first, then the snap uses the sneaking eye height
// (EYE_HEIGHT - 0.3 = 1.3) so the body settles at 65 + 1.3 = 66.3, but the
// *previous* frame already left it at 66.5375 — above that target — so
// `newPos.y <= groundY + eyeHeight` is false and the controller reports
// airborne, then falls back into the same place next frame. Standing still
// while sneaking therefore jitters vertically and flickers onGround_.
static void test_SneakingJittersInsteadOfSettling() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(ctrl.IsOnGround());
  CHECK_EQ(ctrl.pos.y, 65.0f + kEyeHeight);

  // Frame 0: drops to 66.5375 and reports airborne.
  ctrl.Update(0.05f, makeMove(0.0f, 0.0f, false, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, 66.5375f);
  CHECK(!ctrl.IsOnGround());

  // Frame 1: snaps back up to the standing height and reports grounded.
  ctrl.Update(0.05f, makeMove(0.0f, 0.0f, false, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_EQ(ctrl.pos.y, 65.0f + kEyeHeight);
  CHECK(ctrl.IsOnGround());

  // Frame 2 repeats frame 0 — the cycle never converges on the lower height.
  ctrl.Update(0.05f, makeMove(0.0f, 0.0f, false, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, 66.5375f);
  CHECK(!ctrl.IsOnGround());
}

// Releasing sneak returns the body to the standing rest height at once.
static void test_ReleasingSneakRestoresTheStandingHeight() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  ctrl.Update(0.05f, makeMove(0.0f, 0.0f, false, /*sneak=*/true), false,
              glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, 66.5375f);

  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.y, 65.0f + kEyeHeight);
  CHECK(ctrl.IsOnGround());
}

// ---------------------------------------------------------------------------
// PlayerController — block collision
// ---------------------------------------------------------------------------

// A wall at x = 10 spanning y = 65..68, standing on the full y = 64 floor.
Fixture makeWallWorld() {
  return Fixture([](MutableChunk &mc) {
    fillPlane(mc, 64);
    for (int y = 65; y <= 68; ++y)
      for (int z = 0; z < 32; ++z)
        setBlock(mc, 10, y, z, kSolid);
  });
}

static void test_AWallBlocksHorizontalMovementButNotGravity() {
  Fixture fixture = makeWallWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  const float dt = 0.05f;
  ctrl.Update(dt, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK(ctrl.IsOnGround());
  const float x0 = ctrl.pos.x;

  // Push straight into the wall for 30 frames: the body never advances.
  for (int i = 0; i < 30; ++i) {
    ctrl.Update(dt, makeMove(), false, glm::vec3(1.0f, 0.0f, 0.0f),
                glm::vec3(0.0f, 0.0f, -1.0f));
  }
  CHECK_NEAR(ctrl.pos.x, x0);
  // ...and does not tunnel into it either.
  CHECK(ctrl.pos.x < 10.0f);
  // Vertical is never blocked by a wall, and the floor still holds the body at
  // the standing height.
  CHECK_NEAR(ctrl.pos.y, 65.0f + kEyeHeight);
  CHECK(ctrl.IsOnGround());
}

static void test_CollisionResolvesOneAxisAtATime() {
  Fixture fixture = makeWallWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  const float dt = 0.05f;
  ctrl.Update(dt, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  const float x0 = ctrl.pos.x;
  const float z0 = ctrl.pos.z;
  const float step = kSpeed * dt;

  // Strafe with lookRight = (0,0,-1): X is refused by the wall...
  ctrl.Update(dt, makeMove(0.0f, /*right=*/1.0f), false, glm::vec3(1.0f, 0.0f, 0.0f),
              glm::vec3(0.0f, 0.0f, -1.0f));
  CHECK_NEAR(ctrl.pos.x, x0);
  // ...while Z is applied (towards -Z, the direction of lookRight).
  CHECK_NEAR(ctrl.pos.z - z0, -step);
  CHECK(ctrl.IsOnGround());
}

// The body never gets closer than 1.4 blocks to the wall face even when pushed
// continuously: the 0.6-wide bounding box resolves at the nearest cell
// boundary, not at the block surface, so the body stops well short.
static void test_PushingIntoTheWallRepeatedlyDoesNotTunnel() {
  Fixture fixture = makeWallWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};

  const float dt = 0.05f;
  ctrl.Update(dt, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  float last = ctrl.pos.x;
  for (int i = 0; i < 30; ++i) {
    ctrl.Update(dt, makeMove(), false, glm::vec3(1.0f, 0.0f, 0.0f),
                glm::vec3(0.0f, 0.0f, -1.0f));
    // Monotonically non-decreasing and never inside the wall.
    CHECK_GE(ctrl.pos.x, last);
    CHECK(ctrl.pos.x < 10.0f);
    last = ctrl.pos.x;
  }
  // It never moves at all here: the very first candidate step already collides.
  CHECK_EQ(last, 8.5f);
}

// A solid cell directly overhead stops the rise: the head check samples
// floor(pos.y + 0.3) and the cell at y = 68 is taken.
static void test_FlyingIntoCeilingStopsAtTheUnderside() {
  Fixture fixture([](MutableChunk &mc) {
    fillPlane(mc, 64);
    setBlock(mc, 8, 68, 8, kSolid);
  });
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 67.0f, 8.5f};

  const float dt = 0.05f;
  for (int i = 0; i < 8; ++i) {
    ctrl.Update(dt, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
                glm::vec3(1.0f, 0.0f, 0.0f));
    // The body never rises into or through the ceiling cell.
    CHECK(ctrl.pos.y < 68.0f);
  }
  // OBSERVED: the player simply falls to the floor instead — the ceiling block
  // does not lift or stop anything here, it just occupies the cell above.
  CHECK(ctrl.IsOnGround());
  CHECK_NEAR(ctrl.pos.y, 65.0f + kEyeHeight);
}

static void test_UpdateWithZeroDeltaTimeDoesNotMove() {
  Fixture fixture = makeFloorWorld();
  PlayerController ctrl;
  ctrl.SetWorld(&fixture.world);
  ctrl.pos = {8.5f, 66.6f, 8.5f};
  ctrl.Update(0.05f, makeMove(), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  const glm::vec3 grounded = ctrl.pos;

  ctrl.Update(0.0f, makeMove(/*forward=*/1.0f), false, glm::vec3(0.0f, 0.0f, -1.0f),
              glm::vec3(1.0f, 0.0f, 0.0f));
  CHECK_NEAR(ctrl.pos.x, grounded.x);
  CHECK_NEAR(ctrl.pos.z, grounded.z);
  CHECK_NEAR(ctrl.pos.y, grounded.y);
}

} // namespace

int main() {
  TEST(HeldItemIsZeroWithoutInventory);
  TEST(HeldItemFollowsSelectedSlot);
  TEST(HeldItemOfAnEmptyInventoryIsZero);
  TEST(SetInventoryCanClearThePointer);

  TEST(FreshSystemHasNoHighlight);
  TEST(UpdateSetsHighlightFromTheCrosshairRay);
  TEST(UpdateWithNoTargetLeavesTheHighlightOff);
  TEST(UpdateKeepsTheLastBlockAfterLosingTheTarget);
  TEST(HighlightFollowsTheBlockTheCrosshairMovesTo);

  TEST(RaycastTargetMissesOnAnEmptyWorld);
  TEST(RaycastTargetFindsABlockWithinReach);
  TEST(ReachStopsOneCellPastFiveBlocks);
  TEST(TheNearerBlockWinsAndRemovalRevealsTheNext);
  TEST(InsideASolidBlockTheRayHitsItself);
  TEST(MouseRayAgreesWithTheCrosshairRayAtScreenCenter);
  TEST(MouseRayAtAnEdgeLooksDownNotForward);
  TEST(OffCenterMouseRayHitsABlockTheCrosshairMisses);
  TEST(RaycastHitFindsTheImmediatelyAdjacentCell);
  TEST(RaycastHitAgreesWithRaycastTarget);
  TEST(ExtremeAspectStillHitsFromTheScreenCentre);

  TEST(TargetFaceOfTheForwardRayIsSouth);
  TEST(TargetFaceOfAMissIsDown);
  TEST(MouseRayReportsTheAdjacentCellsFourFaces);
  TEST(PositiveXFaceIsUnreachableThroughRaycastHit);
  TEST(TargetFaceDisagreesWithRaycastHitOnTheSameCamera);
  TEST(HitInfoUvFeedsTheWrenchGrid);
  TEST(WrenchGridAgreesWithTheHitFaceInTheCentre);

  TEST(LeftClickMarksTheActionPending);
  TEST(LeftClickIsDebouncedWhileTheActionIsPending);
  TEST(LeftClickWithNoTargetIsIgnored);
  TEST(HeldMouseButtonWithoutThePressEdgeDoesNotBreak);
  TEST(ClickAfterLosingTheTargetCannotBreakAStaleBlock);
  TEST(RightClickIsNotABreakGesture);

  TEST(WrenchGateRunsOnlyWithAHighlight);
  TEST(WrenchGateWithWrenchHeldLeavesTheDebounceAlone);
  TEST(WrenchGateIsSkippedForANonWrenchItem);
  TEST(NoBinderMeansNoWrenchQuery);
  TEST(SpectatorLeftClickIsRefusedInsideInteractionSystem);
  TEST(AdventureLeftClickIsRefusedInsideInteractionSystem);
  TEST(UndefinedGameModeFailsClosedInsideInteractionSystem);
  TEST(NoInventoryResolvesToTheDefaultMode);

  TEST(ControllerStartsAtTheSpawnPoint);
  TEST(FlightMovesAlongTheLookVector);
  TEST(FlightUsesTheFullThreeDimensionalLookVector);
  TEST(FlightVerticalInputMovesOnItsOwn);
  TEST(FlightIsNeverBlockedByBlocks);
  TEST(MissingWorldFallsBackToFlight);
  TEST(ExplicitSetToNullAlsoFallsBackToFlight);

  TEST(WalkFallsUntilItLandsOnTheFloor);
  TEST(StandingStillKeepsThePlayerGrounded);
  TEST(WalkingDoesNotClimbIntoTheGround);
  TEST(WalkingOffALedgeStartsFalling);

  TEST(JumpOnlyAppliesFromTheGround);
  TEST(JumpIsSuppressedWhileSneaking);
  TEST(SneakingInMidAirDoesNotSuppressTheJump);

  TEST(SneakStopsAtALedge);
  TEST(WithoutSneakTheSameStrifeFallsOff);
  TEST(SneakingJittersInsteadOfSettling);
  TEST(ReleasingSneakRestoresTheStandingHeight);

  TEST(AWallBlocksHorizontalMovementButNotGravity);
  TEST(CollisionResolvesOneAxisAtATime);
  TEST(PushingIntoTheWallRepeatedlyDoesNotTunnel);
  TEST(FlyingIntoCeilingStopsAtTheUnderside);
  TEST(UpdateWithZeroDeltaTimeDoesNotMove);

  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
