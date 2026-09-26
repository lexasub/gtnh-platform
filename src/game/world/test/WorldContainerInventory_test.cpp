// Unit tests for simcore::WorldContainerInventory — the world-block container
// (chest / furnace / electrolyser) inventory, its ECS entity lifecycle and its
// persistence round trip through EntityStateStoreClient.
//
// File under test: src/game/world/WorldContainerInventory.cpp
//
// The suite originally (gp-fmeu) pinned the defects as OBSERVED so they could
// be triaged. Four of them have since been FIXED (gp-xck4, gp-x91u, gp-j2gg)
// and the affected assertions were flipped to pin the fixed behaviour; the
// reasoning for each flip is recorded in the comment on the test itself and in
// the FIXED block at the bottom of this file. Tests that were purely a
// restatement of a fixed bug were replaced, not deleted.

//
// DETERMINISM: the storage client is a link-time stub (stub_entity_state_client.cpp)
// that calls its callback synchronously on the calling thread. There is no
// socket, no io_context run, no thread, no timer and no wall-clock waiting, so
// every observation below is a pure function of the inputs. spdlog messages go
// to stderr; the test never asserts on log text.
//
// No file under src/ was modified.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ---- project test harness (mirrors src/game/world/test/BlockTransforms_test.cpp) ----
int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "", msg ? msg : "");
  }
}

#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define TEST(name) do { printf("  TEST: %s\n", #name); test_##name(); } while (0)

#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include <engine/sim/components/InventoryContainer.h>
#include <flatbuffers/flatbuffers.h>
#include <game/world/WorldContainerInventory.h>
#include <machine_state_generated.h>

#include "stub_entity_state_client.h"

using simcore::InventoryContainer;
using simcore::InventorySlot;
using simcore::StubEntityStoreControl;
using simcore::WorldContainerInventory;
using simcore::g_stub;

namespace {

constexpr uint64_t kAlice = 1001;
constexpr uint64_t kBob = 2002;
constexpr uint32_t kX = 10, kY = 70, kZ = -3;

// slotCountForType() is a file-static in WorldContainerInventory.cpp; the test
// restates it (private, so not reachable) and every expectation is written in
// the same expression shape.
uint16_t expectedSlotCount(uint16_t entity_type) {
  switch (entity_type) {
    case 1: return 3;
    case 2: return 9;
    default: return 27;
  }
}

// One registry + one stub-backed client, reset for every test. The io_context
// is never run: the stub's Load/Save call their callback inline, so nothing
// needs a reactor.
struct Fixture {
  entt::registry reg;
  asio::io_context io;
  std::shared_ptr<simcore::EntityStateStoreClient> client;
  std::unique_ptr<WorldContainerInventory> inv;

  explicit Fixture(bool connected = true) {
    g_stub.reset();
    g_stub.connected = connected;
    client = std::make_shared<simcore::EntityStateStoreClient>(io);
    inv = std::make_unique<WorldContainerInventory>(reg, client);
  }
  ~Fixture() { g_stub.reset(); }
};

// Counts the live entities in a fixture's registry, by iterating the component
// storage. This EnTT build has no registry::each(), so storage() is the
// portable way to reach the live entities.
size_t regAlive(Fixture& f) {
  size_t n = 0;
  for (auto [id, pool] : f.reg.storage()) {
    (void)id;
    n += pool.size();
  }
  return n;
}

// Builds a valid MachineState blob holding `slots`, using the same
// serializeToBlob logic production uses (flatbuffers, Protocol namespace), so
// the tests exercise the real round trip rather than a hand-rolled layout.
// `entity_type` is not part of the MachineState wire format — WorldContainer
// stores it in the request, not the blob — so it is accepted for symmetry with
// the call sites and deliberately unused.
std::vector<uint8_t> makeBlob(uint16_t /*entity_type*/, uint16_t slot_count,
                              const std::vector<InventorySlot>& slots,
                              bool with_inventory = true,
                              bool with_slot_vector = true) {
  flatbuffers::FlatBufferBuilder fbb(256);
  if (!with_inventory) {
    auto state = Protocol::CreateMachineState(fbb, 1, 0, 0, 0, 0);
    fbb.Finish(state);
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
  }
  std::vector<flatbuffers::Offset<Protocol::MachineInventorySlot>> offsets;
  for (const auto& s : slots) {
    offsets.push_back(Protocol::CreateMachineInventorySlot(fbb, s.item_id, s.count, s.meta));
  }
  auto slot_vec = with_slot_vector ? fbb.CreateVector(offsets) : 0;
  auto machine_inv = Protocol::CreateMachineInventory(fbb, slot_count, slot_vec);
  auto state = Protocol::CreateMachineState(fbb, 1, 0, 0, machine_inv, 0);
  fbb.Finish(state);
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

} // namespace

// ===========================================================================
// Construction and the null-storage path
// ===========================================================================

static void test_NullStorageIsAcceptedAndOnlyDisablesPersistence() {
  g_stub.reset();
  entt::registry reg;
  // The ctor only warns; it does not throw and does not dereference.
  WorldContainerInventory inv(reg, nullptr);

  // With no storage client, loadContainer cannot ask for a saved state, so it
  // opens the container EMPTY straight away (gp-xck4): the position is usable
  // instead of silently never opening. No load RPC is attempted because there
  // is no client to issue it.
  inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 0u, "and no load is attempted");
  InventoryContainer* c = inv.getContainer(kX, kY, kZ);
  CHECK(c != nullptr,
        "with null storage the container opens EMPTY rather than never at all");
  if (c) {
    CHECK_EQ(c->slot_count, 27u, "still sized by the entity type");
    CHECK_EQ(c->slots.size(), 27u);
  }

  // Actions against it work; a close of a null-storage container is a no-op
  // for the save but must not crash.
  inv.onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  inv.onContainerClose(kAlice, kX, kY, kZ);
  CHECK(true, "actions and closes against a null-storage container are safe");
}

// ===========================================================================
// onContainerOpen — the happy path and the empty-state defect
// ===========================================================================

static void test_OpeningWithSavedStateCreatesTheContainer() {
  Fixture f;
  std::vector<InventorySlot> slots = {
      InventorySlot(101, 5, 0),
      InventorySlot(0, 0, 0),
      InventorySlot(202, 64, 3),
  };
  g_stub.load_state = makeBlob(0, 3, slots);
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);

  CHECK_EQ(g_stub.load_calls.size(), 1u, "one load was issued");
  if (g_stub.load_calls.size() == 1) {
    const auto& call = g_stub.load_calls[0];
    CHECK_EQ(call.dimension, 0, "containers live in dimension 0");
    CHECK_EQ(call.x, static_cast<int32_t>(kX));
    CHECK_EQ(call.y, static_cast<int32_t>(kY));
    CHECK_EQ(call.z, static_cast<int32_t>(kZ), "the z coordinate is sign-extended correctly");
    CHECK_EQ(call.entity_type, 0u);
  }

  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "a container with saved state IS registered");
  if (c) {
    CHECK_EQ(c->entity_type, 0u);
    CHECK_EQ(c->slot_count, 3u, "slot_count is the max of the two decoded sizes");
    CHECK_EQ(c->slots.size(), 3u, "the three saved slots were decoded");
    if (c->slots.size() == 3) {
      CHECK_EQ(c->slots[0].item_id, 101u);
      CHECK_EQ(c->slots[0].count, 5u);
      CHECK_EQ(c->slots[0].meta, 0u);
      CHECK_EQ(c->slots[1].item_id, 0u, "an empty slot decodes as item 0");
      CHECK_EQ(c->slots[2].item_id, 202u);
      CHECK_EQ(c->slots[2].count, 64u);
      CHECK_EQ(c->slots[2].meta, 3u, "a non-zero meta survives the round trip");
    }
  }
}

static void test_EmptySavedStateOpensAnEmptyContainer() {
  // gp-xck4 (was: test_EmptyStateLeavesTheContainerUnopenable, which pinned
  // the BUG). WorldContainerInventory.cpp:204 used to be
  //     if (stat.state.empty())  {  /* only logs "using empty" */ }
  //     else { ...create the entity, populate open_containers_... }
  // so the empty branch opened NOTHING and a freshly placed chest — which by
  // definition has no saved state — could never be opened.
  //
  // THE FIX opens the empty container. "No saved state" is the NORMAL reply
  // (EntityStateData::state is empty for an unset position), and it is also
  // exactly what the real client reports when it cannot reach entitystated
  // (EntityStateStoreClient.cpp:54), so the two cases are indistinguishable
  // from here. Refusing either would keep a chest unopenable. The container
  // is therefore opened empty and sized by the entity type, which is what
  // ChestOpenHandler.cpp:32-40 and MachineOpenHandler.cpp:78-92 already do
  // ("session must exist before any click arrives").
  Fixture f;
  g_stub.load_state.clear();  // storage says "no saved state"

  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);

  CHECK_EQ(g_stub.load_calls.size(), 1u, "the load was still issued");

  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "an empty saved state opens an EMPTY container, not nothing");
  if (c) {
    CHECK_EQ(c->entity_type, 0u);
    CHECK_EQ(c->slot_count, 27u, "sized by the entity type, not by the (absent) blob");
    CHECK_EQ(c->slots.size(), 27u, "with a full complement of empty slots");
    for (const auto& s : c->slots) CHECK_EQ(s.item_id, 0u, "and every slot empty");
  }

  // The whole point: the player can now actually put items in the new chest.
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 27, 0, /*count=*/0);
  InventoryContainer* after = f.inv->getContainer(kX, kY, kZ);
  CHECK(after != nullptr, "an action against the new container is not refused");
  if (after) {
    const InventorySlot moved = after->getSlot(27);
    CHECK_EQ(moved.item_id, 0u, "the source slot was empty, so nothing moved");
  }

  // A second open is now a genuine duplicate and issues no further load.
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 1u,
           "the container is remembered as open, so re-opening is a no-op");
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr, "and survives the duplicate open");
}

static void test_EmptySavedStateIsSizedByTheEntityType() {
  // The empty branch must size the container from slotCountForType, exactly
  // as the non-empty branch's pre-sized fallback would.
  for (uint16_t type : {uint16_t{0}, uint16_t{1}, uint16_t{2}, uint16_t{99}}) {
    Fixture f;
    g_stub.load_state.clear();
    f.inv->onContainerOpen(kAlice, kX, kY, kZ, type);
    InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
    CHECK(c != nullptr, "a container opened for this entity type");
    if (c) {
      CHECK_EQ(c->entity_type, type);
      CHECK_EQ(c->slot_count, expectedSlotCount(type));
      CHECK_EQ(c->slots.size(), static_cast<size_t>(expectedSlotCount(type)));
    }
  }
}

static void test_AFreshlyOpenedEmptyContainerSurvivesTheSaveRoundTrip() {
  // open(empty) -> mutate -> close -> reopen must give back the mutated
  // contents. Before gp-xck4 the open never happened, so this path was
  // unreachable: a new chest could not be filled and therefore never had
  // anything to save.
  Fixture f;
  g_stub.load_state.clear();
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 2);  // type 2 -> 9 slots
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "the new container opened");
  if (c) c->setSlot(4, InventorySlot(123, 6, 2));

  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 1u, "closing the new container saved it");
  if (g_stub.save_calls.empty()) return;

  g_stub.load_state = g_stub.save_calls[0].state;
  g_stub.load_calls.clear();
  f.inv->onContainerOpen(kBob, kX, kY, kZ, 2);
  InventoryContainer* reloaded = f.inv->getContainer(kX, kY, kZ);
  CHECK(reloaded != nullptr, "and it reopened");
  if (reloaded) {
    CHECK_EQ(reloaded->slots.size(), 9u, "with the type's slot count preserved");
    CHECK_EQ(reloaded->slots[4].item_id, 123u, "the item put in is still there");
    CHECK_EQ(reloaded->slots[4].count, 6u);
    CHECK_EQ(reloaded->slots[4].meta, 2u);
  }
}

static void test_ReopeningAnAlreadyOpenContainerIsANoOp() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(7, 1, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr, "the first open works");

  f.inv->onContainerOpen(kBob, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 1u, "a second open issues no further load");
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "the container survives the duplicate open");
  if (c && c->slots.size() == 1) {
    CHECK_EQ(c->slots[0].item_id, 7u, "and its contents are untouched");
  }
}

static void test_ContainersAtDifferentPositionsAreIndependent() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(7, 1, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerOpen(kAlice, kX + 1, kY, kZ, 0);
  f.inv->onContainerOpen(kAlice, kX, kY, kZ + 1, 0);

  CHECK_EQ(g_stub.load_calls.size(), 3u, "three distinct positions, three loads");
  InventoryContainer* a = f.inv->getContainer(kX, kY, kZ);
  InventoryContainer* b = f.inv->getContainer(kX + 1, kY, kZ);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ + 1);
  CHECK(a != nullptr && b != nullptr && c != nullptr, "all three are registered");
  // Distinct entities, not three views of one.
  CHECK(a != b, "different positions get different entities");
  CHECK(a != c, "different positions get different entities");

  if (a && b) {
    a->setSlot(0, InventorySlot(999, 3, 0));
    CHECK_EQ(b->slots[0].item_id, 7u, "writing one does not disturb another");
  }
  CHECK(f.inv->getContainer(kX + 2, kY, kZ) == nullptr, "an unopened position is null");
}

static void test_PositionKeyHandlesNegativeAndLargeCoordinates() {
  // gp-x91u FIXED. packKey used to pack x,y,z as three 21-bit fields with no
  // bias, so a coordinate at or past 2^21 aliased onto a different position.
  // This test used to assert the collision (it is what pinned the bug); the
  // collisions themselves are now covered by
  // test_PositionsBeyondTheTwentyOneBitFieldDoNotAlias. What remains here is
  // the ordinary-coordinate contract: a negative z is a plain two's-complement
  // uint32_t and reuses its own key without aliasing anything.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(7, 1, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* first = f.inv->getContainer(kX, kY, kZ);
  CHECK(first != nullptr, "the container opened");

  // -3 == 0xFFFFFFFD, a perfectly ordinary uint32_t, so it keys cleanly.
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 1u, "a negative z reuses the same key, no aliasing");

  // A neighbouring position that the old encoder could have confused with it.
  f.inv->onContainerOpen(kAlice, kX, kY, 0u, 0);
  f.inv->onContainerOpen(kAlice, kX, kY + 1, kZ, 0);
  f.inv->onContainerOpen(kAlice, kX + 1, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 4u,
           "and each neighbouring position keys independently");
  CHECK(f.inv->getContainer(kX, kY, 0u) != nullptr);
  CHECK(f.inv->getContainer(kX, kY + 1, kZ) != nullptr);
  CHECK(f.inv->getContainer(kX + 1, kY, kZ) != nullptr);
}

// ===========================================================================
// Slot counts per entity type
// ===========================================================================

static void test_SlotCountDependsOnEntityType() {
  for (uint16_t type : {uint16_t{0}, uint16_t{1}, uint16_t{2}, uint16_t{3}, uint16_t{99}}) {
    Fixture f;
    // A blob whose slot_count field disagrees with the type, so the decoded
    // value comes from the BLOB, not from slotCountForType.
    g_stub.load_state = makeBlob(type, expectedSlotCount(type),
                                 std::vector<InventorySlot>(expectedSlotCount(type)));
    f.inv->onContainerOpen(kAlice, kX, kY, kZ, type);
    InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
    CHECK(c != nullptr, "a container opened for this entity type");
    if (c) {
      CHECK_EQ(c->entity_type, type, "the entity type round trips");
      CHECK_EQ(c->slot_count, expectedSlotCount(type),
               "slot_count matches the type's expected size");
    }
  }
}

static void test_SavedSlotCountOverridesTheEntityTypeDefault() {
  // slotCountForType() computes a size, but onContainerOpen only uses it to
  // size the temporary container it passes to loadContainer. Once a blob is
  // deserialized, slot_count comes from the BLOB and can be anything.
  Fixture f;
  g_stub.load_state = makeBlob(0, 12, std::vector<InventorySlot>(12));
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);  // type 0 would mean 27
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (c) {
    CHECK_EQ(c->slot_count, 12u, "the blob's slot_count wins over the type default of 27");
  }
}

// ===========================================================================
// deserializeFromBlob — malformed input
// ===========================================================================

static void test_GarbageBlobLeavesTheContainerAtItsDefaults() {
  // deserializeFromBlob verifies the buffer and returns early on failure,
  // leaving the caller's container as-is. Because loadContainer starts from a
  // correctly-sized empty container, the container still opens — with slots
  // sized by the entity type rather than by the (unreadable) blob.
  Fixture f;
  g_stub.load_state = {'n', 'o', 't', ' ', 'a', ' ', 'b', 'l', 'o', 'b'};
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "an unparseable blob still opens the container (defaults kept)");
  if (c) {
    CHECK_EQ(c->entity_type, 0u);
    CHECK_EQ(c->slot_count, 27u, "slot_count falls back to the entity type default of 27");
    CHECK_EQ(c->slots.size(), 27u, "with a full complement of empty slots");
    for (const auto& s : c->slots) CHECK_EQ(s.item_id, 0u, "and every slot empty");
  }
}

static void test_BlobWithoutAnInventoryOpensAnEmptyContainer() {
  // state->inventory() is null -> deserializeFromBlob returns before touching
  // slot_count, so the entity-type default survives.
  Fixture f;
  g_stub.load_state = makeBlob(0, 0, {}, /*with_inventory=*/false);
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 2);  // type 2 -> 9 slots
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "a MachineState with no inventory still opens the container");
  if (c) {
    CHECK_EQ(c->slot_count, 9u, "the entity type default of 9 is kept");
    CHECK_EQ(c->slots.size(), 9u);
  }
}

static void test_BlobWithAnInventoryButNoSlotVectorOpensAnEmptyContainer() {
  // The inv->slots() null check: a MachineInventory with a null slot vector.
  Fixture f;
  g_stub.load_state = makeBlob(0, 5, {}, /*with_inventory=*/true, /*with_slot_vector=*/false);
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "a null slot vector still opens the container");
  if (c) {
    CHECK_EQ(c->slot_count, 27u, "the entity type default is kept when slots is null");
  }
}

static void test_EmptySlotVectorYieldsZeroSlots() {
  // A present-but-empty slot vector DOES clear the slots, so the container
  // ends up with fewer slots than the entity type implies.
  Fixture f;
  g_stub.load_state = makeBlob(0, 0, {});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (c) {
    CHECK_EQ(c->slot_count, 0u, "max(size=0, slots->size()=0) is 0");
    CHECK_EQ(c->slots.size(), 0u, "and the slot list is emptied, unlike the null-vector case");
  }
}

static void test_SlotCountIsTheMaxOfTheTwoSizes() {
  // slot_count = max(inv->size(), slots->size()). A blob claiming a larger
  // size() than it has slots keeps the larger number, leaving a container
  // whose slot_count overstates its actual slots.
  Fixture f;
  std::vector<InventorySlot> two = {InventorySlot(1, 1, 0), InventorySlot(2, 2, 0)};
  g_stub.load_state = makeBlob(0, 40, two);
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (c) {
    CHECK_EQ(c->slot_count, 40u, "slot_count takes the larger of size() and slots->size()");
    CHECK_EQ(c->slots.size(), 2u, "but only two slots actually exist");
  }
}

static void test_ZeroLengthStateTakesTheEmptyBranchNotTheGarbageBranch() {
  // Distinct from GarbageBlobLeavesTheContainerAtItsDefaults: a 0-byte reply
  // never reaches the verifier, it is caught by the state.empty() branch. The
  // two branches now converge on the same result — an open, type-sized
  // container — but they are still reached by different code paths, and the
  // 0-byte reply must NOT be run through the flatbuffers verifier.
  Fixture f;
  g_stub.load_state.clear();
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "a 0-byte state takes the empty branch, not the garbage branch");
  if (c) {
    CHECK_EQ(c->slot_count, 27u, "so the entity type default is kept");
    CHECK_EQ(c->slots.size(), 27u);
  }
}

// ===========================================================================
// onContainerAction — MOVE (action 0)
// ===========================================================================

static void test_MoveIntoAnEmptySlotRelocatesTheWholeStack() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 3, {InventorySlot(5, 12, 2), InventorySlot(0, 0, 0), InventorySlot(9, 1, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;

  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/0);
  InventoryContainer* after = f.inv->getContainer(kX, kY, kZ);
  CHECK(after != nullptr, "the container is still there after a move");
  if (!after) return;
  // gp-x91u FIXED (two flips). This test used to send count=1 and expect the
  // whole 12-item stack to move: the old code discarded `count` outright, so
  // that assertion pinned the discarded-count bug. count == 0 is now the
  // "unspecified amount" sentinel that means "the whole stack" for MOVE.
  // It also expected the source slot to be ERASED (slots.size() 3 -> 2),
  // which pinned removeItem's erase-on-zero and the slot shift that came with
  // it; the source is now emptied in place, because a slot index is a wire
  // address the client already addressed.
  CHECK_EQ(after->slots.size(), 3u, "draining the source does not shrink the container");
  CHECK_EQ(after->getSlot(0).item_id, 0u, "the source is an empty slot, not a removed one");
  CHECK_EQ(after->getSlot(1).item_id, 5u, "the stack moved to the destination");
  CHECK_EQ(after->getSlot(1).count, 12u);
  CHECK_EQ(after->getSlot(1).meta, 2u);
  CHECK_EQ(after->getSlot(2).item_id, 9u, "and the stack below it did NOT shift down");
  CHECK_EQ(after->getSlot(2).count, 1u);
  CHECK_EQ(after->slot_count, 3u, "slot_count tracks the vector");
}

static void test_MoveFromAnEmptySlotDoesNothing() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 3, {InventorySlot(0, 0, 0), InventorySlot(4, 4, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  const size_t before = c->slots.size();

  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);  // src is empty
  InventoryContainer* after = f.inv->getContainer(kX, kY, kZ);
  CHECK(after != nullptr);
  if (after) {
    CHECK_EQ(after->slots.size(), before, "moving from an empty slot changes nothing");
    CHECK_EQ(after->slots[1].item_id, 4u, "the destination is untouched");
  }
}

static void test_MoveOntoTheSameItemMergesUpToSixtyFour() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 50, 0), InventorySlot(5, 10, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // gp-x91u: count=1 here used to mean "move everything" (the argument was
  // discarded); 0 is now the explicit "whole stack" sentinel. And the emptied
  // source is emptied in place rather than erased, so the merge no longer
  // slides the destination down into index 0.
  CHECK_EQ(c->slots.size(), 2u, "the emptied source stays as a slot");
  CHECK_EQ(c->getSlot(0).item_id, 0u, "the source is empty");
  CHECK_EQ(c->getSlot(1).item_id, 5u);
  CHECK_EQ(c->getSlot(1).count, 60u, "10 + 50 merged into 60 in the DESTINATION slot");
  CHECK_EQ(c->slot_count, 2u, "slot_count tracks the vector");
}

static void test_MergeIntoAFullStackSwapsInstead() {
  // A full destination (count == 64) fails the `d.count < 64` guard, so the
  // MOVE falls through to the SWAP branch and the two stacks trade places.
  // The 64-item stack ends up in the source slot and the small one in the
  // destination — not a merge, and no items are lost. QUIRK, not a bug: the
  // caller asked to move, and a swap is the defined behaviour.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(5, 64, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "a swap erases nothing");
  CHECK_EQ(c->slots[0].count, 64u, "the full stack moved INTO the source slot");
  CHECK_EQ(c->slots[1].count, 3u, "and the small stack took the destination");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[1].item_id, 5u);
}

static void test_MergeIsCappedAtSixtyFourAndDropsTheRemainder() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 60, 0), InventorySlot(5, 30, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // space = 64 - 30 = 34, moved = min(34, 60) = 34. dst becomes 64 and the
  // source keeps 60 - 34 = 26. Nothing is lost. (count=0 is the "whole stack"
  // sentinel; the old code ignored the argument entirely.)
  CHECK_EQ(c->slots.size(), 2u, "the partially-drained source is kept, not erased");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[0].count, 26u, "the source keeps what did not fit");
  CHECK_EQ(c->slots[1].item_id, 5u);
  CHECK_EQ(c->slots[1].count, 64u, "the destination is filled to the cap");
}

static void test_MoveOntoADifferentItemSwapsThem() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 1), InventorySlot(9, 7, 2)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "a swap erases nothing");
  CHECK_EQ(c->slots[0].item_id, 9u, "the stacks exchanged places");
  CHECK_EQ(c->slots[0].count, 7u);
  CHECK_EQ(c->slots[0].meta, 2u, "the meta travels with the stack");
  CHECK_EQ(c->slots[1].item_id, 5u);
  CHECK_EQ(c->slots[1].count, 3u);
  CHECK_EQ(c->slots[1].meta, 1u);
}

static void test_MovingOntoTheSameSlotLeavesItUnchanged() {
  // src == dst. The old code was accidentally idempotent here only because
  // its doubled write was undone by the erase-shift that followed; with the
  // drain fixed it is idempotent by construction, because both reads name the
  // same slot and every write is followed by an equal drain from that same
  // slot. Still pinned: a self-move must never duplicate or erase a stack.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(9, 7, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 0, 0);  // src == dst
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "no slot is erased or added");
  CHECK_EQ(c->getSlot(0).item_id, 5u);
  CHECK_EQ(c->getSlot(0).count, 3u, "the stack keeps its original count");
  CHECK_EQ(c->getSlot(0).meta, 0u);
  CHECK_EQ(c->getSlot(1).item_id, 9u, "and the other stack is intact");
  CHECK_EQ(c->getSlot(1).count, 7u);
  CHECK_EQ(c->slot_count, 2u, "slot_count is unchanged");
}

static void test_OutOfRangeSlotIndexIsTreatedAsEmpty() {
  // InventoryContainer::getSlot returns an empty slot for index >= size,
  // and setSlot RESIZES to fit. So a move to a far slot grows the container
  // and can copy an item into it, but a move FROM a far slot is a no-op.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 99, 1, 1);  // src 99 is empty
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "a move from an out-of-range slot changes nothing");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[1].item_id, 0u);
}

static void test_MoveToAnOutOfRangeSlotGrowsTheContainer() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 40, /*count=*/0);  // dst 40
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // gp-x91u FIXED. This used to expect the stack at index 39 with 40 entries,
  // because removeItem erased the drained source and shifted everything down;
  // and it expected slot_count left stale at 2. Both were the bug: a slot
  // index is a wire address, so the stack must sit in the slot the client
  // addressed (40), and slot_count must track the vector — otherwise the
  // inconsistent pair is what gets serialised to entitystated.
  CHECK_EQ(c->slots.size(), 41u, "the vector grew to reach the destination slot");
  CHECK_EQ(c->getSlot(40).item_id, 5u, "the stack is in the slot the client addressed");
  CHECK_EQ(c->getSlot(40).count, 3u);
  CHECK_EQ(c->getSlot(40).meta, 0u);
  CHECK_EQ(c->getSlot(0).item_id, 0u, "the source is emptied in place");
  for (size_t i = 1; i < 40; ++i) {
    CHECK_EQ(c->getSlot(static_cast<uint16_t>(i)).item_id, 0u,
             "the padding slots are empty");
  }
  CHECK_EQ(c->slot_count, 41u, "slot_count is updated with the vector, not left stale");
}

// ===========================================================================
// onContainerAction — SPLIT (action 1)
// ===========================================================================

static void test_SplitHalvesTheStackIntoAnEmptySlot() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 9, 4), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // half = (9 + 1) / 2 = 5. Source keeps 4, destination gets 5.
  // (count=1 here used to mean "split it in half" because the argument was
  // discarded; 0 is now the explicit "half" sentinel for SPLIT.)
  CHECK_EQ(c->slots.size(), 2u, "an odd split leaves the source non-empty, so nothing changes");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[0].count, 4u, "the source keeps the floor half");
  CHECK_EQ(c->slots[1].item_id, 5u);
  CHECK_EQ(c->slots[1].count, 5u, "the destination gets the rounded-up half");
  CHECK_EQ(c->slots[1].meta, 4u, "the split destination keeps the meta");
  // The 9 items become 4 + 5: a split of an odd stack is exact here because
  // half = (count + 1) / 2 and removeItem(half) leaves count - half.
}

static void test_SplitRoundsUpForAnEvenStack() {
  // half = (8 + 1) / 2 = 4 (integer division). 4 + 4 = 8, exact.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 8, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u);
  CHECK_EQ(c->getSlot(0).count, 4u);
  CHECK_EQ(c->getSlot(1).count, 4u);
}

static void test_SplitOfAStackOfOneIntoAnOccupiedSlotSwaps() {
  // half = (1 + 1) / 2 = 1. The destination holds a DIFFERENT item, so
  // neither SPLIT branch matched and the old code fell through doing nothing
  // — which silently lost the item from the client's point of view.
  // gp-x91u FIXED: an incompatible destination now swaps, like MOVE's.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 1, 0), InventorySlot(9, 1, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "nothing was added or removed");
  CHECK_EQ(c->getSlot(0).item_id, 9u, "the stacks exchanged places");
  CHECK_EQ(c->getSlot(0).count, 1u);
  CHECK_EQ(c->getSlot(1).item_id, 5u);
  CHECK_EQ(c->getSlot(1).count, 1u, "the single item is not lost");
}

static void test_SplitOfASingleItemIntoAnEmptySlotIsExact() {
  // half = 1, so the single item is written to the destination and then the
  // source is erased. Net effect: the item moved, nothing duplicated.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 1, 7), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // gp-x91u: the old code ERASED the drained source, so the size came out
  // unchanged and the item appeared to stay in slot 0. The source is now
  // emptied in place, which is the observable difference: the item is
  // genuinely in the destination slot the client addressed.
  CHECK_EQ(c->slots.size(), 2u, "the drained source stays as a slot");
  CHECK_EQ(c->getSlot(0).item_id, 0u, "the source is empty");
  CHECK_EQ(c->getSlot(1).item_id, 5u, "the single item moved to the destination");
  CHECK_EQ(c->getSlot(1).count, 1u);
  CHECK_EQ(c->getSlot(1).meta, 7u, "with its meta intact");
}

static void test_SplitFromAnEmptySlotDoesNothing() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(0, 0, 0), InventorySlot(4, 4, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "the container is unchanged");
  CHECK_EQ(c->getSlot(1).item_id, 4u);
  CHECK_EQ(c->getSlot(1).count, 4u);
}

static void test_SplitOntoTheSameItemMerges() {
  // half = (10 + 1) / 2 = 5, space = 64 - 2 = 62, moved = min(5, 62) = 5.
  // dst becomes 7 and removeItem(0, 5) leaves 5 in the source — not zero, so
  // the source is NOT erased. 5 + 5 + 2 = 12: nothing is lost.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 10, 0), InventorySlot(5, 2, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "the partially-drained source is kept");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[0].count, 5u, "the source keeps half");
  CHECK_EQ(c->slots[1].item_id, 5u);
  CHECK_EQ(c->slots[1].count, 7u, "the destination gained the other half");
}

// ===========================================================================
// gp-x91u — container action item loss, the discarded count argument,
// slot_count drift and position-key aliasing
// ===========================================================================

static void test_SplitDoesNotShiftSlotsWhenTheSourceIsDrained() {
  // gp-x91u. InventoryContainer::removeItem ERASES the slot when the count
  // hits zero (InventoryContainer.h:72-74), so every later slot slides down
  // by one. Splitting the LAST item out of slot 0 into slot 3 therefore
  // erased index 0 and left the freshly-placed stack at index 2: the client
  // showed the item in slot 3, the server holds it in slot 2, and the next
  // save/reload shifts the whole container's slot identity.
  //
  // A container slot index is a wire address, so a drain must write an EMPTY
  // slot, never remove one.
  Fixture f;
  g_stub.load_state = makeBlob(0, 4, {InventorySlot(5, 1, 0), InventorySlot(0, 0, 0),
                                      InventorySlot(0, 0, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 3, /*count=*/0);

  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 4u, "draining a slot does not shrink the container");
  CHECK_EQ(c->getSlot(0).item_id, 0u, "the drained source is an EMPTY slot, not a removed one");
  CHECK_EQ(c->getSlot(3).item_id, 5u, "the stack lands in the slot the client clicked");
  CHECK_EQ(c->getSlot(3).count, 1u);
  CHECK_EQ(c->getSlot(3).meta, 0u);
  CHECK_EQ(c->slot_count, 4u, "slot_count survives the drain");
}

static void test_MoveDoesNotShiftSlotsWhenTheSourceIsDrained() {
  // Same defect through the MOVE path, which is the one a plain left-click
  // drag takes.
  Fixture f;
  g_stub.load_state = makeBlob(0, 4, {InventorySlot(5, 1, 0), InventorySlot(0, 0, 0),
                                      InventorySlot(0, 0, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 3, /*count=*/0);

  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 4u, "draining a slot does not shrink the container");
  CHECK_EQ(c->getSlot(0).item_id, 0u, "the drained source is emptied in place");
  CHECK_EQ(c->getSlot(3).item_id, 5u, "the stack lands in the destination slot");
  CHECK_EQ(c->slot_count, 4u, "slot_count survives the drain");
}

static void test_SplitOntoADifferentItemIsNotSilentlySwallowed() {
  // gp-x91u. The SPLIT branch had no fallback: a destination holding a
  // different item (or a full stack of the same item) matched neither case, so
  // the action did NOTHING while the client — which has already decremented
  // its own slot optimistically (DragManager.cpp:35-48) — believed the half
  // had moved. The half vanished from the player's point of view.
  //
  // FIXED: the incompatible destination takes the same swap MOVE already uses
  // for it, so the two stacks exchange places. That is the only outcome that
  // neither loses the half nor contradicts the client's local state, and it
  // keeps one rule for "destination cannot accept a partial" in both actions.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 8, 0), InventorySlot(9, 3, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);

  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "nothing was added or removed");
  CHECK_EQ(c->getSlot(0).item_id, 9u, "the stacks exchanged places");
  CHECK_EQ(c->getSlot(0).count, 3u);
  CHECK_EQ(c->getSlot(1).item_id, 5u);
  CHECK_EQ(c->getSlot(1).count, 8u);
  CHECK_EQ(c->getSlot(1).meta, 0u);
}

static void test_SplitOntoAFullSameItemStackSwapsInsteadOfLosingTheHalf() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 8, 0), InventorySlot(5, 64, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "no item is lost into a full destination");
  CHECK_EQ(c->getSlot(0).count, 64u, "the full stack took the source slot");
  CHECK_EQ(c->getSlot(1).count, 8u, "and the small stack took the destination");
}

static void test_AnExplicitCountMovesExactlyThatManyItems() {
  // gp-x91u: `count` was discarded outright ((void)count at the old line 86).
  // The client sends a real amount (DragManager.cpp:107 sends 1 for a
  // right-click place-one, :157 sends the merge amount), so a UI asking for a
  // partial stack had it silently upgraded to the whole stack.
  //
  // FIXED: count == 0 means "unspecified" (whole stack for MOVE, half for
  // SPLIT, as before); a non-zero count is the exact number to move, clamped
  // to what the source actually holds.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 30, 7), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/5);

  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "a partial move neither adds nor removes a slot");
  CHECK_EQ(c->getSlot(0).item_id, 5u);
  CHECK_EQ(c->getSlot(0).count, 25u, "only the requested 5 left the source");
  CHECK_EQ(c->getSlot(0).meta, 7u, "and the meta stayed with the remainder");
  CHECK_EQ(c->getSlot(1).item_id, 5u);
  CHECK_EQ(c->getSlot(1).count, 5u, "the destination got exactly 5");
  CHECK_EQ(c->getSlot(1).meta, 7u, "meta travels with the moved items");
}

static void test_AnExplicitCountSplitsExactlyThatManyItems() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 9, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, /*count=*/3);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->getSlot(0).count, 6u, "an explicit count overrides the half rule");
  CHECK_EQ(c->getSlot(1).count, 3u, "and moves exactly that many");
}

static void test_ACountLargerThanTheStackIsClampedNotFabricated() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 4, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/200);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->getSlot(0).count, 0u, "the source is fully drained");
  CHECK_EQ(c->getSlot(1).count, 4u, "and only the 4 items that existed moved — none invented");
}

static void test_ACountOfOneMovesASingleItemRatherThanDuplicating() {
  // The "place one" gesture the client actually sends on a right-click
  // (DragManager.cpp:107).
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 10, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->getSlot(0).count, 9u, "one item left the source");
  CHECK_EQ(c->getSlot(1).count, 1u, "and one arrived");
  CHECK_EQ(c->getSlot(0).count + c->getSlot(1).count, 10u,
           "the stack total is conserved exactly");
}

static void test_SlotCountStaysInSyncWithTheSlotVector() {
  // gp-x91u. slot_count came from the blob and was never revisited, while
  // setSlot resizes the vector and removeItem erased from it. One move to an
  // out-of-range index left the container claiming 2 slots while holding 40 —
  // and that inconsistent pair is what gets serialised to entitystated.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 40, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slot_count, c->slots.size(),
           "slot_count always equals the number of slots actually held");
  CHECK_EQ(c->getSlot(40).item_id, 5u, "the stack is in the requested slot");
  CHECK_EQ(c->getSlot(40).count, 3u);
  CHECK_EQ(c->getSlot(0).item_id, 0u, "the source is emptied in place");
}

static void test_PositionsBeyondTheTwentyOneBitFieldDoNotAlias() {
  // gp-x91u. packKey squeezed x,y,z into 21-bit fields with no bias and no
  // range check, so a coordinate at or past 2^21 spilled into the neighbouring
  // field and silently aliased a DIFFERENT container — which then could never
  // be opened, and whose close released the wrong chest.
  //
  // The exact collisions of the old key (x<<42 | y<<21 | z, truncated to 64
  // bits) are: x >= 2^22 wraps entirely away, x = 2^22.. aliases x = 0;
  // y = 2^21 lands where x = 1, y = 0; z = 2^21 lands where y = 1, z = 0.
  // Each pair below is a real collision of the old encoder.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(7, 1, 0)});

  // x = 2^22 (one past the usable range) vs x = 0.
  const uint32_t kXOverflow = 1u << 22;
  f.inv->onContainerOpen(kAlice, kXOverflow, kY, kZ, 0);
  f.inv->onContainerOpen(kAlice, 0, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 2u, "an out-of-range x and x=0 issue two loads");
  InventoryContainer* big = f.inv->getContainer(kXOverflow, kY, kZ);
  InventoryContainer* zero = f.inv->getContainer(0, kY, kZ);
  CHECK(big != nullptr, "the out-of-range container opened");
  CHECK(zero != nullptr, "and so did the one it used to alias onto");
  CHECK(big != zero, "they are genuinely different containers");

  // Closing one must not release the other.
  f.inv->onContainerClose(kAlice, 0, kY, kZ);
  CHECK(f.inv->getContainer(0, kY, kZ) == nullptr, "closing one closes only that one");
  CHECK(f.inv->getContainer(kXOverflow, kY, kZ) != nullptr,
        "the other is untouched, not released by aliasing");
}

static void test_LargeSignedCoordinatesAreDistinctPositions() {
  // The realistic case of the same defect: a world whose far coordinates pass
  // 2^21 in every axis (the default overworld is ±30M in the usual modded
  // setups, well past 2^21 = 2M) must not collide. Negative values arrive as
  // plain uint32_t two's-complement and must stay distinct too.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(7, 1, 0)});
  const uint32_t kBig = 30000000;  // ~3e7: beyond 21 bits, inside int32
  const uint32_t kNegBig = 0u - 30000000;
  // kZ is already -3, so a second negative z has to be a different value.
  const uint32_t kNegZ = 0u - 4;
  const uint32_t kNegZ2 = 0u - 9;

  f.inv->onContainerOpen(kAlice, kBig, kY, kZ, 0);
  f.inv->onContainerOpen(kAlice, kNegBig, kY, kZ, 0);
  f.inv->onContainerOpen(kAlice, kBig, kY, kNegZ, 0);
  f.inv->onContainerOpen(kAlice, kNegBig, kY, kNegZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 4u, "four distinct positions, four loads");
  CHECK(f.inv->getContainer(kBig, kY, kZ) != nullptr);
  CHECK(f.inv->getContainer(kNegBig, kY, kZ) != nullptr);
  CHECK(f.inv->getContainer(kBig, kY, kNegZ) != nullptr);
  CHECK(f.inv->getContainer(kNegBig, kY, kNegZ) != nullptr);

  // Adjacent far-apart positions must not collide either.
  f.inv->onContainerOpen(kAlice, kBig + 1, kY, kZ, 0);
  f.inv->onContainerOpen(kAlice, kBig, kY + 1, kZ, 0);
  f.inv->onContainerOpen(kAlice, kBig, kY, kNegZ2, 0);
  CHECK_EQ(g_stub.load_calls.size(), 7u,
           "and their one-off neighbours stay distinct at the far end of the range");
}

static void test_NeverAnyActionCreatesOrDestroysAnItem() {
  // The conservation invariant behind all of gp-x91u, checked across the
  // whole action matrix: the total number of items in the container is
  // invariant, and the two stacks involved always end up on the two slots
  // the client addressed.
  struct Case { uint8_t action, src, dst, count; };
  for (Case c : {Case{0, 0, 1, 0}, Case{0, 0, 1, 3}, Case{1, 0, 1, 0},
                 Case{1, 0, 1, 2}, Case{0, 0, 0, 0}, Case{1, 0, 0, 0}}) {
    Fixture f;
    g_stub.load_state = makeBlob(
        0, 3, {InventorySlot(5, 7, 0), InventorySlot(9, 2, 0), InventorySlot(5, 64, 0)});
    f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
    InventoryContainer* before = f.inv->getContainer(kX, kY, kZ);
    if (!before) { CHECK(false, "container open for the matrix case"); continue; }
    size_t total_before = 0;
    for (const auto& s : before->slots) total_before += s.count;

    f.inv->onContainerAction(kAlice, kX, kY, kZ, c.action, c.src, c.dst, c.count);

    InventoryContainer* after = f.inv->getContainer(kX, kY, kZ);
    if (!after) { CHECK(false, "container survives the matrix case"); continue; }
    size_t total_after = 0;
    for (const auto& s : after->slots) total_after += s.count;
    CHECK_EQ(total_after, total_before,
             "no action creates or destroys an item");
    CHECK_EQ(after->slots.size(), 3u, "and no action resizes the container");
    CHECK_EQ(after->slot_count, 3u, "and slot_count tracks it");
  }
}

// ===========================================================================
// onContainerAction — access control and unknown actions
// ===========================================================================

static void test_ActionFromAnotherPlayerIsRejected() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kBob, kX, kY, kZ, 0, 0, 0, 0);  // Bob is not the opener
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (c) {
    CHECK_EQ(c->slots[0].count, 3u, "another player's action changed nothing");
  }
}

static void test_ActionOnAnUnopenedContainerIsRejected() {
  Fixture f;
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr, "no container was conjured up");
  CHECK(true, "the rejected action did not crash");
}

static void test_UnknownActionIsIgnored() {
  for (uint8_t action : {uint8_t{2}, uint8_t{7}, uint8_t{255}}) {
    Fixture f;
    g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(0, 0, 0)});
    f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
    f.inv->onContainerAction(kAlice, kX, kY, kZ, action, 0, 1, 1);
    InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
    CHECK(c != nullptr, "the container survives an unknown action");
    if (c) {
      CHECK_EQ(c->slots.size(), 2u, "and its contents are unchanged");
      CHECK_EQ(c->slots[0].count, 3u);
    }
  }
}

static void test_CountZeroMeansTheWholeStack() {
  // gp-x91u: `count` was discarded outright (`(void)count;`). The client sends
  // a real amount on every gesture (DragManager.cpp:28,48,107,157), so the
  // only way a "move the whole stack" intent can be expressed now is
  // count == 0, the "unspecified amount" sentinel. A plain left-click drag
  // from an old client that sent 0 keeps its old meaning, and one that sent a
  // real count now gets exactly that count.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 30, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "count=0 still moves the whole 30-item stack");
  CHECK_EQ(c->getSlot(0).item_id, 0u, "the source is emptied in place");
  CHECK_EQ(c->getSlot(1).count, 30u, "and all 30 land in the destination");
}

// ===========================================================================
// onContainerClose — the save path
// ===========================================================================

static void test_CloseSerialisesTheContainerAndReleasesTheEntity() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 1), InventorySlot(9, 4, 2)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr, "the container is open");

  // Mutate it, then close.
  InventoryContainer* live = f.inv->getContainer(kX, kY, kZ);
  CHECK(live != nullptr);
  if (live) live->setSlot(0, InventorySlot(42, 11, 6));

  f.inv->onContainerClose(kAlice, kX, kY, kZ);

  CHECK_EQ(g_stub.save_calls.size(), 1u, "closing issued exactly one save");
  if (g_stub.save_calls.size() == 1) {
    const auto& call = g_stub.save_calls[0];
    CHECK_EQ(call.dimension, 0);
    CHECK_EQ(call.x, static_cast<int32_t>(kX));
    CHECK_EQ(call.y, static_cast<int32_t>(kY));
    CHECK_EQ(call.z, static_cast<int32_t>(kZ));
    CHECK_EQ(call.entity_type, 0u, "the container's entity type is stored");
    CHECK(!call.state.empty(), "a non-empty blob was sent");
  }

  // The save callback ran inline and destroyed the entity + removed the key.
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr, "the container is closed");
}

static void test_SavedBlobDecodesBackToTheSameContents() {
  // The full persistence round trip: mutate, close, reopen, and the contents
  // must be identical. This is the contract that makes the class useful.
  Fixture f;
  std::vector<InventorySlot> original = {
      InventorySlot(5, 3, 1), InventorySlot(0, 0, 0), InventorySlot(9, 64, 2),
  };
  g_stub.load_state = makeBlob(0, 3, original);
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);

  InventoryContainer* live = f.inv->getContainer(kX, kY, kZ);
  CHECK(live != nullptr);
  if (live) {
    live->setSlot(0, InventorySlot(77, 21, 13));
    live->setSlot(1, InventorySlot(88, 2, 0));
  }
  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 1u, "the close saved once");
  if (g_stub.save_calls.empty()) return;

  // Feed the saved blob back in, as a reconnect would.
  g_stub.load_state = g_stub.save_calls[0].state;
  g_stub.load_calls.clear();
  f.inv->onContainerOpen(kBob, kX, kY, kZ, 0);
  InventoryContainer* reloaded = f.inv->getContainer(kX, kY, kZ);
  CHECK(reloaded != nullptr, "the container reopened from the saved blob");
  if (reloaded) {
    CHECK_EQ(reloaded->slots.size(), 3u);
    if (reloaded->slots.size() == 3) {
      CHECK_EQ(reloaded->slots[0].item_id, 77u);
      CHECK_EQ(reloaded->slots[0].count, 21u);
      CHECK_EQ(reloaded->slots[0].meta, 13u);
      CHECK_EQ(reloaded->slots[1].item_id, 88u);
      CHECK_EQ(reloaded->slots[1].count, 2u);
      CHECK_EQ(reloaded->slots[2].item_id, 9u);
      CHECK_EQ(reloaded->slots[2].count, 64u);
      CHECK_EQ(reloaded->slots[2].meta, 2u);
    }
  }
}

static void test_CloseByAnotherPlayerIsRejected() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerClose(kBob, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 0u, "no save was issued");
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr,
        "the container is still open for its real owner");
}

static void test_CloseOfAnUnopenedContainerIsRejected() {
  Fixture f;
  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 0u, "no save was issued");
  CHECK(true, "the rejected close did not crash");
}

static void test_ReopenAfterCloseSucceeds() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr);
  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr, "closed");

  g_stub.load_calls.clear();
  f.inv->onContainerOpen(kBob, kX, kY, kZ, 0);
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr, "a different player can reopen it");
  CHECK_EQ(g_stub.load_calls.size(), 1u, "and the close really released the key");
}

static void test_RegistryIsUnchangedByARejectedClose() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  const size_t before = regAlive(f);
  f.inv->onContainerClose(kBob, kX, kY, kZ);  // wrong player
  CHECK_EQ(regAlive(f), before, "a rejected close destroys no ECS entity");
  f.inv->onContainerClose(kAlice, kX, kY, kZ);  // the real owner
  CHECK_EQ(regAlive(f), before - 1, "the accepted close destroys exactly one entity");
}

// ===========================================================================
// gp-j2gg — release-on-close is conditional on the save actually landing
// ===========================================================================

static void test_FailedSaveKeepsTheContainerOpenSoThePlayerCanRetry() {
  // gp-j2gg. The old callback ignored its `res` argument apart from an error
  // log and ALWAYS destroyed the entity and erased the open_containers_ key.
  // A transient entitystated outage therefore destroyed the only copy of the
  // container's contents: the player had put items in, closed the window, and
  // the items were gone with nothing but a log line to show for it.
  //
  // FIXED: a failed save KEEPS the container open and registered, so the
  // player still holds their items and a subsequent close retries the write.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  g_stub.save_ok = false;
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "the container is open");
  if (c) c->setSlot(0, InventorySlot(5, 9, 0));

  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 1u, "the save was attempted");

  InventoryContainer* after = f.inv->getContainer(kX, kY, kZ);
  CHECK(after != nullptr, "a FAILED save keeps the container open, contents intact");
  if (after) {
    CHECK_EQ(after->getSlot(0).count, 9u, "the items are still there to retry with");
  }

  // And the retry works: the second close succeeds and then releases.
  g_stub.save_ok = true;
  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 2u, "closing again retries the save");
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr,
        "and a successful save does release the container");
}

static void test_AFailedSaveDoesNotLetAnotherPlayerStealTheContainer() {
  // The container is still registered after a failed save, so the
  // player_id check must still gate it: a retry is only the original opener's
  // to make.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  g_stub.save_ok = false;
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr, "still open after the failure");

  g_stub.save_calls.clear();
  f.inv->onContainerClose(kBob, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 0u, "another player cannot close it");
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr, "and it stays with its owner");
}

static void test_AFailedSaveDoesNotWedgeThePositionForever() {
  // The opposite failure to the null-storage one: a failed save must not make
  // the position permanently unopenable. The container is still registered (so
  // its contents survive), but the player must be able to act on it again.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  g_stub.save_ok = false;
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerClose(kAlice, kX, kY, kZ);

  g_stub.load_calls.clear();
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 0u,
           "a still-registered container is a duplicate open, not a new load");
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (c) {
    // And the player can keep using it, then close again.
    c->setSlot(0, InventorySlot(6, 4, 0));
    CHECK_EQ(c->getSlot(0).item_id, 6u, "the container is still fully usable");
  }
  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 2u, "and closing again retries the write");
}

static void test_UnconnectedStorageReleaseIsNotDelayedForever() {
  // gp-j2gg, second half. With a null storage client saveContainer returned
  // BEFORE the RPC, so the entity and the open_containers_ entry were never
  // released: the container stayed open for the process lifetime and every
  // later onContainerOpen at that position was swallowed by the
  // "already open" guard — the position was permanently wedged.
  //
  // FIXED: with no storage client there is nothing to persist and no way ever
  // to persist it, so the close releases the container immediately instead of
  // holding it forever. (Note this only releases; the contents are simply not
  // saved, which is the honest outcome of having no storage at all.)
  g_stub.reset();
  entt::registry reg;
  {
    WorldContainerInventory inv(reg, nullptr);  // NO storage client at all
    inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
    CHECK(inv.getContainer(kX, kY, kZ) != nullptr, "opened with a null storage client");

    inv.onContainerClose(kAlice, kX, kY, kZ);
    CHECK(inv.getContainer(kX, kY, kZ) == nullptr,
          "a close with NO storage client releases instead of wedging");
  }
  // And the position is usable again afterwards — a fresh instance opens it.
  g_stub.connected = true;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(7, 1, 0)});
  {
    entt::registry reg2;
    asio::io_context io;
    auto client = std::make_shared<simcore::EntityStateStoreClient>(io);
    WorldContainerInventory inv(reg2, client);
    inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
    CHECK(inv.getContainer(kX, kY, kZ) != nullptr,
          "and the position is immediately re-openable");
  }
}

static void test_ADisconnectedClientKeepsTheContainerBecauseTheSaveFailed() {
  // The distinction gp-j2gg draws: a client that EXISTS but cannot reach
  // entitystated produces a FAILED save, so the container is kept (the
  // player's items are safe and a retry can succeed once entitystated is
  // back). That is deliberately different from having no client at all, where
  // the close can never succeed and holding the container forever would wedge
  // the position.
  g_stub.reset();
  g_stub.connected = false;
  entt::registry reg;
  asio::io_context io;
  auto client = std::make_shared<simcore::EntityStateStoreClient>(io);
  WorldContainerInventory inv(reg, client);

  inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(inv.getContainer(kX, kY, kZ) != nullptr, "opened with a disconnected client");
  inv.onContainerClose(kAlice, kX, kY, kZ);
  CHECK(inv.getContainer(kX, kY, kZ) != nullptr,
        "a save that FAILED keeps the container, so the items are not lost");

  // entitystated comes back and the retry lands.
  g_stub.connected = true;
  inv.onContainerClose(kAlice, kX, kY, kZ);
  CHECK(inv.getContainer(kX, kY, kZ) == nullptr,
        "and the retried save releases it once it succeeds");
}

static void test_AFailedSaveUnderAConnectedClientKeepsTheContainerToo() {
  // Same as the first test but reached the realistic way: entitystated is up
  // (so the open worked) and then the WRITE fails, e.g. a disk error.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(9, 4, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  g_stub.save_ok = false;
  f.inv->onContainerClose(kAlice, kX, kY, kZ);

  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr, "a rejected write does not destroy the live container");
  if (c) {
    CHECK_EQ(c->slots.size(), 2u, "with every slot intact");
    CHECK_EQ(c->getSlot(1).item_id, 9u, "including the untouched ones");
  }
  const size_t alive = regAlive(f);
  CHECK(alive > 0, "the ECS entity survives a failed save");
}

// ===========================================================================
// The unconnected storage path
// ===========================================================================

static void test_UnconnectedStorageOpensAnEmptyContainer() {
  // gp-xck4. The stub reproduces the real client's unconnected behaviour
  // exactly (spdlog::error + callback with an EMPTY state), which used to
  // drive WorldContainerInventory straight into a branch that opened
  // NOTHING — so a simcore that starts before entitystated is up could never
  // open ANY container.
  //
  // FIXED: because the empty state and the unconnected reply are literally the
  // same callback payload (EntityStateStoreClient.cpp:54), the container now
  // opens empty. Contents will be restored on the next open once entitystated
  // answers, which is exactly the trade the sibling handlers already make.
  g_stub.reset();
  g_stub.connected = false;
  entt::registry reg;
  asio::io_context io;
  auto client = std::make_shared<simcore::EntityStateStoreClient>(io);
  WorldContainerInventory inv(reg, client);

  inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 1u, "the load was attempted");
  InventoryContainer* c = inv.getContainer(kX, kY, kZ);
  CHECK(c != nullptr,
        "unconnected storage opens an EMPTY container, not nothing");
  if (c) {
    CHECK_EQ(c->slot_count, 27u, "sized by the entity type");
    CHECK_EQ(c->slots.size(), 27u);
  }

  // The player can interact with it; a save still fails, and the release
  // policy on failure is gp-j2gg's subject, not this test's.
  inv.onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  CHECK(inv.getContainer(kX, kY, kZ) != nullptr, "and an action is not refused");
}

static void test_UnconnectedStorageMakesEverySaveFail() {
  g_stub.reset();
  g_stub.connected = false;
  entt::registry reg;
  asio::io_context io;
  auto client = std::make_shared<simcore::EntityStateStoreClient>(io);
  WorldContainerInventory inv(reg, client);

  // A container that IS open (loaded with a connected client, then the
  // connection drops) still saves, and the save fails.
  g_stub.connected = true;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(inv.getContainer(kX, kY, kZ) != nullptr, "the container is open");

  g_stub.connected = false;  // entitystated restarts
  inv.onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 1u, "the save was attempted");
  // gp-j2gg FIXED. This used to expect the container to be released anyway,
  // "losing its contents" — the bug. A disconnected client reports a FAILED
  // save, so the container is now kept and the player's items survive until
  // entitystated is back.
  CHECK(inv.getContainer(kX, kY, kZ) != nullptr,
        "a failed save keeps the container, so an entitystated outage does not eat the items");
}

// ===========================================================================
// FIXED (were bugs, each with a RED->GREEN test in this file)
//
//  gp-xck4  a container with no saved state could never be opened. The
//           empty branch at the old :204 only logged "using empty" and
//           created no entity, so every freshly placed chest was
//           unopenable — and so was EVERY container when simcore started
//           before entitystated, because the real client reports
//           "unconnected" as an empty state (EntityStateStoreClient.cpp:54),
//           indistinguishable from "no saved state". Both now open an empty,
//           type-sized container, matching ChestOpenHandler.cpp:32-40 and
//           MachineOpenHandler.cpp:78-92. Tests:
//           EmptySavedStateOpensAnEmptyContainer,
//           EmptySavedStateIsSizedByTheEntityType,
//           AFreshlyOpenedEmptyContainerSurvivesTheSaveRoundTrip,
//           UnconnectedStorageOpensAnEmptyContainer.
//
//  gp-x91u  Four defects in onContainerAction, all reachable from a normal
//           client click:
//           1. SPLIT onto an incompatible destination (different item, or a
//              full same-item stack) had no fallback and silently dropped the
//              half. It now swaps, exactly as MOVE already did.
//           2. `count` was discarded (`(void)count;`), so a client could not
//              move a partial stack. count == 0 is now the "unspecified
//              amount" sentinel (whole stack for MOVE, half for SPLIT, as
//              before); a non-zero count moves exactly that many, clamped to
//              the source.
//           3. removeItem ERASED a drained slot, sliding every later slot
//              down one, so an item the client had placed in slot N ended up
//              in slot N-1. A slot index is a wire address, so a drain now
//              writes an empty slot in place (drainSlot).
//           4. slot_count drifted from slots.size() and the inconsistent pair
//              was persisted. slot_count is now resynced after every action.
//           5. packKey used three unbias'd 21-bit fields, so a coordinate at
//              or past 2^21 aliased a different position — reachable in normal
//              play, since a ±30M overworld is well past 2^21 = 2M. Each axis
//              now gets its own 32 bits, so the key is injective over int32.
//
//  gp-j2gg  A failed save released the container anyway, and a null storage
//           client never released it at all (permanently wedging the
//           position). See the section above for the fixed contract.
//
//  STILL OPEN, unchanged by the fixes above:
//   - a MOVE whose destination is a FULL stack of the same item falls through
//     to the swap branch, so the two stacks trade places rather than merging.
//     Defined behaviour, not a loss.
//   - BLOCKDROPS, NOT HERE: BlockDrops never used d->meta (gp-pukq notes it
//     under BreakBlockHandler), which is a drop-handler concern, not a
//     container one.
// ===========================================================================

int main() {
  TEST(NullStorageIsAcceptedAndOnlyDisablesPersistence);

  TEST(OpeningWithSavedStateCreatesTheContainer);
  TEST(EmptySavedStateOpensAnEmptyContainer);
  TEST(EmptySavedStateIsSizedByTheEntityType);
  TEST(AFreshlyOpenedEmptyContainerSurvivesTheSaveRoundTrip);
  TEST(ReopeningAnAlreadyOpenContainerIsANoOp);
  TEST(ContainersAtDifferentPositionsAreIndependent);
  TEST(PositionKeyHandlesNegativeAndLargeCoordinates);

  TEST(SlotCountDependsOnEntityType);
  TEST(SavedSlotCountOverridesTheEntityTypeDefault);

  TEST(GarbageBlobLeavesTheContainerAtItsDefaults);
  TEST(BlobWithoutAnInventoryOpensAnEmptyContainer);
  TEST(BlobWithAnInventoryButNoSlotVectorOpensAnEmptyContainer);
  TEST(EmptySlotVectorYieldsZeroSlots);
  TEST(SlotCountIsTheMaxOfTheTwoSizes);
  TEST(ZeroLengthStateTakesTheEmptyBranchNotTheGarbageBranch);

  TEST(MoveIntoAnEmptySlotRelocatesTheWholeStack);
  TEST(MoveFromAnEmptySlotDoesNothing);
  TEST(MoveOntoTheSameItemMergesUpToSixtyFour);
  TEST(MergeIntoAFullStackSwapsInstead);
  TEST(MergeIsCappedAtSixtyFourAndDropsTheRemainder);
  TEST(MoveOntoADifferentItemSwapsThem);
  TEST(MovingOntoTheSameSlotLeavesItUnchanged);
  TEST(OutOfRangeSlotIndexIsTreatedAsEmpty);
  TEST(MoveToAnOutOfRangeSlotGrowsTheContainer);

  TEST(SplitHalvesTheStackIntoAnEmptySlot);
  TEST(SplitRoundsUpForAnEvenStack);
  TEST(SplitOfAStackOfOneIntoAnOccupiedSlotSwaps);
  TEST(SplitOfASingleItemIntoAnEmptySlotIsExact);
  TEST(SplitFromAnEmptySlotDoesNothing);
  TEST(SplitOntoTheSameItemMerges);
  TEST(SplitOntoADifferentItemIsNotSilentlySwallowed);
  TEST(SplitOntoAFullSameItemStackSwapsInsteadOfLosingTheHalf);

  TEST(SplitDoesNotShiftSlotsWhenTheSourceIsDrained);
  TEST(MoveDoesNotShiftSlotsWhenTheSourceIsDrained);
  TEST(AnExplicitCountMovesExactlyThatManyItems);
  TEST(AnExplicitCountSplitsExactlyThatManyItems);
  TEST(ACountLargerThanTheStackIsClampedNotFabricated);
  TEST(ACountOfOneMovesASingleItemRatherThanDuplicating);
  TEST(SlotCountStaysInSyncWithTheSlotVector);
  TEST(PositionsBeyondTheTwentyOneBitFieldDoNotAlias);
  TEST(LargeSignedCoordinatesAreDistinctPositions);
  TEST(NeverAnyActionCreatesOrDestroysAnItem);

  TEST(ActionFromAnotherPlayerIsRejected);
  TEST(ActionOnAnUnopenedContainerIsRejected);
  TEST(UnknownActionIsIgnored);
  TEST(CountZeroMeansTheWholeStack);

  TEST(CloseSerialisesTheContainerAndReleasesTheEntity);
  TEST(SavedBlobDecodesBackToTheSameContents);
  TEST(CloseByAnotherPlayerIsRejected);
  TEST(CloseOfAnUnopenedContainerIsRejected);
  TEST(FailedSaveKeepsTheContainerOpenSoThePlayerCanRetry);
  TEST(AFailedSaveDoesNotLetAnotherPlayerStealTheContainer);
  TEST(AFailedSaveDoesNotWedgeThePositionForever);
  TEST(AFailedSaveUnderAConnectedClientKeepsTheContainerToo);
  TEST(UnconnectedStorageReleaseIsNotDelayedForever);
  TEST(ADisconnectedClientKeepsTheContainerBecauseTheSaveFailed);
  TEST(ReopenAfterCloseSucceeds);
  TEST(RegistryIsUnchangedByARejectedClose);

  TEST(UnconnectedStorageOpensAnEmptyContainer);
  TEST(UnconnectedStorageMakesEverySaveFail);

  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
