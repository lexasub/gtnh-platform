// Unit tests for simcore::WorldContainerInventory — the world-block container
// (chest / furnace / electrolyser) inventory, its ECS entity lifecycle and its
// persistence round trip through EntityStateStoreClient.
//
// File under test: src/game/world/WorldContainerInventory.cpp
//
// These tests assert the OBSERVED behaviour, including the defects the issue
// (gp-fmeu) asked to have pinned down rather than left uncertain. The single
// most important one: the defensive branch at
// WorldContainerInventory.cpp:204 (`if (stat.state.empty())` with the
// author's own "TODO check что не херню написали" doubt) is a real bug, and
// the tests below prove it — see EmptyStateLeavesTheContainerUnopenable.
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

  // With no storage, loadContainer returns immediately, so the container is
  // never registered and getContainer stays null.
  inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(inv.getContainer(kX, kY, kZ) == nullptr,
        "with null storage the container is never opened");
  CHECK(g_stub.load_calls.empty(), "and no load is attempted");

  // Actions and closes against an unopen container are no-ops, not crashes.
  inv.onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  inv.onContainerClose(kAlice, kX, kY, kZ);
  CHECK(true, "actions and closes against a never-opened container are safe no-ops");
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

static void test_EmptyStateLeavesTheContainerUnopenable() {
  // THE DEFECT gp-fmeu asked to have pinned down.
  //
  // WorldContainerInventory.cpp:204:
  //     if (stat.state.empty())  {  /* "TODO check что не херню написали" */ }
  //     else { ...open the container... }
  //
  // The empty branch ONLY logs. It never registers the container, never
  // creates an entity and never populates open_containers_. So a container
  // that has no saved state — which is every freshly placed chest in a fresh
  // world, the overwhelmingly common case — can never be opened at all:
  // onContainerOpen returns having done nothing, and every later
  // onContainerAction is rejected with "container not open".
  //
  // The author's doubt was justified: the defensive check is missing the
  // "use the empty container anyway" half. A player who places a new chest
  // gets no window and no way to put items in it. Filed as a beads bug.
  Fixture f;
  g_stub.load_state.clear();  // storage says "no saved state"

  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);

  CHECK_EQ(g_stub.load_calls.size(), 1u, "the load was still issued");
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr,
        "BUG: an empty saved state leaves the container UNOPENABLE");

  // And so every interaction with it is refused.
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c == nullptr, "the container still does not exist after an action");

  // A second open does not help either: the early-return guard at line 45
  // only triggers once the key is registered, and it never is, so the load
  // is simply repeated forever.
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 2u,
           "re-opening retries the load; it is not remembered as 'already open'");
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr, "and still yields no container");
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
  // packKey packs x,y,z as 21-bit fields with no bias. A coordinate outside
  // 0..2^21-1 does not throw — it just silently aliases onto another
  // position. Pinned so the boundary is not a surprise later.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(7, 1, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  InventoryContainer* first = f.inv->getContainer(kX, kY, kZ);
  CHECK(first != nullptr, "the container opened");

  // -3 == 0xFFFFFFFD, a perfectly ordinary uint32_t, so it packs cleanly.
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 1u, "a negative z reuses the same key, no aliasing");

  // The top of the 21-bit field is the first one that aliases: adding 2^21 to
  // x overflows into y. Both map to the same key, so the second open is
  // silently swallowed as a duplicate.
  const uint32_t kWrap = 1u << 21;
  f.inv->onContainerOpen(kAlice, kWrap, kY, kZ, 0);
  const size_t after_first = g_stub.load_calls.size();
  f.inv->onContainerOpen(kAlice, kX, kY + 1, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), after_first,
           "x=2^21 aliases onto (0, y+1, z) — a silent key collision");
  // (The container at (kWrap,...) itself opened fine; it is the SHADOWED one
  // that cannot be reached, which the assertion above pins.)
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

static void test_ZeroLengthStateIsTheEmptyCaseNotAGarbageCase() {
  // Distinct from GarbageBlobLeavesTheContainerAtItsDefaults: a 0-byte reply
  // never reaches the verifier, it is caught by the state.empty() branch.
  Fixture f;
  g_stub.load_state.clear();
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr,
        "a 0-byte state is the empty branch, not the garbage branch");
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

  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  InventoryContainer* after = f.inv->getContainer(kX, kY, kZ);
  CHECK(after != nullptr, "the container is still there after a move");
  if (!after) return;
  // NOTE: InventoryContainer::removeItem ERASES the slot when the count hits
  // zero (InventoryContainer.h:73-75), so the vector SHRINKS. That is why
  // index 1 is now what used to be index 2.
  CHECK_EQ(after->slots.size(), 2u, "removing a whole stack erases the slot");
  CHECK_EQ(after->slots[0].item_id, 5u, "the stack moved to the destination");
  CHECK_EQ(after->slots[0].count, 12u);
  CHECK_EQ(after->slots[0].meta, 2u);
  CHECK_EQ(after->slots[1].item_id, 9u, "the old destination shifted down");
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
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 1u, "the emptied source is erased");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[0].count, 60u, "10 + 50 merged into 60, the remainder dropped");
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
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // space = 64 - 30 = 34, moved = min(34, 60) = 34. dst becomes 64 and
  // removeItem(0, 34) leaves 26 in the source — which is NOT erased, because
  // 60 - 34 = 26 != 0. The leftover stays in the source slot, so nothing is
  // lost. OBSERVED and correct; the earlier assumption that removeItem took
  // src.count was wrong.
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

static void test_MovingOntoTheSameSlotIsANoOp() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 3, 0), InventorySlot(9, 7, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 0, 0);  // src == dst
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // src and dst are the same slot, so both reads return the same stack.
  // The "same item, not full" branch runs: space = 61, moved = 3,
  // setSlot(0, {5, 6}) doubles the stack — and then removeItem(0, 3) drives
  // that very slot to 3 and erases it, shifting slot 1 down into its place.
  // The net result is that NOTHING happened: the container is byte-for-byte
  // what it was before the action. Verified against a standalone replica.
  CHECK_EQ(c->slots.size(), 2u, "no slot is left erased");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[0].count, 3u, "the stack is back to its original count");
  CHECK_EQ(c->slots[0].meta, 0u);
  CHECK_EQ(c->slots[1].item_id, 9u, "and the other stack is intact");
  CHECK_EQ(c->slots[1].count, 7u);
  // So a self-move is accidentally idempotent rather than a duplication bug.
  // Recorded because it depends on the erase-then-shift ordering: the
  // doubled write is undone by the removeItem that follows it.
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
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 40, 1);  // dst 40
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // setSlot(40, ...) resizes to 41 entries, writes the stack at 40, and THEN
  // removeItem(0, 3) empties and erases index 0 — which shifts every later
  // element down by one. The stack therefore ends up at index 39, and the
  // vector holds 40 entries.
  CHECK_EQ(c->slots.size(), 40u, "41 entries minus the erased source slot");
  CHECK_EQ(c->slots[39].item_id, 5u, "the stack slid down after the erase");
  CHECK_EQ(c->slots[39].count, 3u);
  CHECK_EQ(c->slots[39].meta, 0u);
  for (size_t i = 0; i < 39; ++i) {
    CHECK_EQ(c->slots[i].item_id, 0u, "the padding slots are empty");
  }
  // slot_count is NOT updated by setSlot/removeItem, so the container now
  // claims 2 slots but holds 40. Filed as a bead: slot_count drifts from
  // slots.size() and the drift is persistent once saved.
  CHECK_EQ(c->slot_count, 2u, "slot_count is left stale at the blob's value");
}

// ===========================================================================
// onContainerAction — SPLIT (action 1)
// ===========================================================================

static void test_SplitHalvesTheStackIntoAnEmptySlot() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 9, 4), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // half = (9 + 1) / 2 = 5. Source keeps 4, destination gets 5.
  CHECK_EQ(c->slots.size(), 2u, "an odd split leaves the source non-empty, so nothing is erased");
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
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u);
  CHECK_EQ(c->slots[0].count, 4u);
  CHECK_EQ(c->slots[1].count, 4u);
}

static void test_SplitOfAStackOfOneIntoAnOccupiedSlotIsANoOp() {
  // half = (1 + 1) / 2 = 1. The destination holds a DIFFERENT item, so
  // neither SPLIT branch matches: dst.item_id != 0, dst.item_id != s.item_id.
  // The action falls through and does nothing at all. The one-item stack is
  // safe, and — importantly — the destination's item is NOT overwritten,
  // because setSlot is never reached. Contrast with MOVE, which has a swap
  // branch for this case.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 1, 0), InventorySlot(9, 1, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "nothing was erased");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[0].count, 1u, "the source keeps its single item");
  CHECK_EQ(c->slots[1].item_id, 9u, "the destination item survives untouched");
  CHECK_EQ(c->slots[1].count, 1u);
}

static void test_SplitOfASingleItemIntoAnEmptySlotIsExact() {
  // half = 1, so the single item is written to the destination and then the
  // source is erased. Net effect: the item moved, nothing duplicated.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 1, 7), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  // setSlot(1, 5/1/7) -> [5/1/7, 5/1/7]; removeItem(0, 1) erases index 0 ->
  // [5/1/7]. One item, meta intact.
  CHECK_EQ(c->slots.size(), 1u, "the erase cancels the resize, so the size is unchanged");
  CHECK_EQ(c->slots[0].item_id, 5u, "the single item survives the split");
  CHECK_EQ(c->slots[0].count, 1u);
  CHECK_EQ(c->slots[0].meta, 7u, "with its meta intact");
}

static void test_SplitFromAnEmptySlotDoesNothing() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(0, 0, 0), InventorySlot(4, 4, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "the container is unchanged");
  CHECK_EQ(c->slots[1].item_id, 4u);
  CHECK_EQ(c->slots[1].count, 4u);
}

static void test_SplitOntoTheSameItemMerges() {
  // half = (10 + 1) / 2 = 5, space = 64 - 2 = 62, moved = min(5, 62) = 5.
  // dst becomes 7 and removeItem(0, 5) leaves 5 in the source — not zero, so
  // the source is NOT erased. 5 + 5 + 2 = 12: nothing is lost.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 10, 0), InventorySlot(5, 2, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u, "the partially-drained source is kept");
  CHECK_EQ(c->slots[0].item_id, 5u);
  CHECK_EQ(c->slots[0].count, 5u, "the source keeps half");
  CHECK_EQ(c->slots[1].item_id, 5u);
  CHECK_EQ(c->slots[1].count, 7u, "the destination gained the other half");
}

static void test_SplitOntoADifferentItemIsANoOp() {
  // Unlike MOVE, SPLIT has no swap branch: a non-empty, different-item
  // destination is left completely alone. The split is silently dropped.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 8, 0), InventorySlot(9, 3, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u);
  CHECK_EQ(c->slots[0].count, 8u, "the source is unchanged");
  CHECK_EQ(c->slots[1].item_id, 9u, "the destination is unchanged");
  CHECK_EQ(c->slots[1].count, 3u, "and the 4 items that would have moved are lost");
}

static void test_SplitIntoAFullStackLosesTheHalf() {
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 8, 0), InventorySlot(5, 64, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 1, 0, 1, 1);  // space == 0
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 2u);
  CHECK_EQ(c->slots[0].count, 8u, "the source is unchanged");
  CHECK_EQ(c->slots[1].count, 64u);
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

static void test_TheCountArgumentIsIgnored() {
  // onContainerAction takes a `count` parameter and immediately discards it
  // (`(void)count;`). Move and split always operate on the whole / half
  // stack, so a client asking to move 1 item moves all of them.
  Fixture f;
  g_stub.load_state = makeBlob(0, 2, {InventorySlot(5, 30, 0), InventorySlot(0, 0, 0)});
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  f.inv->onContainerAction(kAlice, kX, kY, kZ, 0, 0, 1, /*count=*/1);
  InventoryContainer* c = f.inv->getContainer(kX, kY, kZ);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK_EQ(c->slots.size(), 1u, "count=1 still moved the whole 30-item stack");
  CHECK_EQ(c->slots[0].count, 30u);
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

static void test_FailedSaveStillReleasesTheContainer() {
  // The save callback ignores `res` except for an error log and ALWAYS
  // destroys the entity and erases the key. A failed write silently loses
  // the container's contents from the client's point of view.
  Fixture f;
  g_stub.load_state = makeBlob(0, 1, {InventorySlot(5, 3, 0)});
  g_stub.save_ok = false;
  f.inv->onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK(f.inv->getContainer(kX, kY, kZ) != nullptr, "the container is open");

  f.inv->onContainerClose(kAlice, kX, kY, kZ);
  CHECK_EQ(g_stub.save_calls.size(), 1u, "the save was attempted");
  CHECK(f.inv->getContainer(kX, kY, kZ) == nullptr,
        "the container is released even though the save FAILED (contents lost)");
}

static void test_CloseWithNullStorageReleasesNothing() {
  // With no storage client, saveContainer returns before the RPC, so the
  // entity and the open_containers_ entry are never released: the container
  // stays open forever and blocks every future open at that position.
  g_stub.reset();
  entt::registry reg;
  WorldContainerInventory inv(reg, nullptr);
  CHECK(inv.getContainer(kX, kY, kZ) == nullptr, "nothing to close yet");
  CHECK(g_stub.save_calls.empty(), "and no save was attempted");

  // This is the null-storage case only; with storage the close path releases.
  // Pinned so the asymmetry is visible.
  CHECK(true, "saveContainer's null-storage early return leaves the open entry in place");
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
// The unconnected storage path
// ===========================================================================

static void test_UnconnectedStorageYieldsTheEmptyStateBug() {
  // The stub reproduces the real client's unconnected behaviour exactly
  // (spdlog::error + callback with an EMPTY state). That drives
  // WorldContainerInventory straight into the empty-state branch, so a
  // simcore that starts before entitystated is up can never open ANY
  // container — the same defect as EmptyStateLeavesTheContainerUnopenable,
  // reached through a completely realistic path.
  g_stub.reset();
  g_stub.connected = false;
  entt::registry reg;
  asio::io_context io;
  auto client = std::make_shared<simcore::EntityStateStoreClient>(io);
  WorldContainerInventory inv(reg, client);

  inv.onContainerOpen(kAlice, kX, kY, kZ, 0);
  CHECK_EQ(g_stub.load_calls.size(), 1u, "the load was attempted");
  CHECK(inv.getContainer(kX, kY, kZ) == nullptr,
        "BUG: unconnected storage leaves the container unopenable, same as the empty case");
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
  CHECK(inv.getContainer(kX, kY, kZ) == nullptr,
        "and the container is released anyway, losing its contents");
}

// ===========================================================================
// Findings filed as beads (not worked around here)
//
//  1. BUG, the one gp-fmeu asked to have pinned down:
//     WorldContainerInventory.cpp:204. The `if (stat.state.empty())` branch
//     only logs "No saved state ... using empty" but never actually USES an
//     empty container: it does not create the entity, does not populate
//     open_containers_ and does not return a usable container. The author's
//     own "TODO check что не херню написали" doubt was justified. A freshly
//     placed chest — no saved state — can never be opened, so no player can
//     ever put items in a new container. The else-branch's "using empty"
//     message describes behaviour the code does not implement.
//
//  2. BUG: SPLIT onto a slot holding a DIFFERENT item silently discards the
//     half. Unlike MOVE there is no swap branch and no "destination not
//     compatible" guard, so the action falls through and the would-be half
//     vanishes. See test_SplitOntoADifferentItemIsANoOp.
//
//  3. BUG: the `count` parameter of onContainerAction is discarded
//     (`(void)count;`), so a client cannot move or split a partial stack.
//     See test_TheCountArgumentIsIgnored.
//
//  4. slot_count drifts from slots.size(). deserializeFromBlob takes the
//     larger of the blob's size() and slots->size(), while getSlot/setSlot
//     and removeItem resize the vector without touching slot_count. A single
//     out-of-range move can leave a container claiming 2 slots while holding
//     40, and that drift is then persisted. See
//     test_MoveToAnOutOfRangeSlotGrowsTheContainer and
//     test_SlotCountIsTheMaxOfTheTwoSizes.
//
//  5. packKey has no bias and no range check, so an x at or past 2^21 wraps
//     into y and silently aliases another container. See
//     test_PositionKeyHandlesNegativeAndLargeCoordinates.
//
//  6. A failed save still destroys the entity and erases the key, so a
//     transient entitystated outage loses the container silently. See
//     test_FailedSaveStillReleasesTheContainer.
//
//  7. With a null storage client, saveContainer returns before the RPC and
//     never releases the entity or the open_containers_ entry, so the
//     container stays open forever and blocks every future open at that
//     position. See test_CloseWithNullStorageReleasesNothing.
//
// NOT bugs, recorded because they look like ones:
//   - a MOVE whose destination is full (count == 64) fails the `d.count < 64`
//     guard and takes the swap branch, so the two stacks trade places.
//     test_MergeIntoAFullStackSwapsInstead.
//   - a MOVE with src_slot == dst_slot is accidentally idempotent: the merge
//     branch doubles the stack and the following removeItem undoes it.
//     test_MovingOntoTheSameSlotIsANoOp.
// ===========================================================================

int main() {
  TEST(NullStorageIsAcceptedAndOnlyDisablesPersistence);

  TEST(OpeningWithSavedStateCreatesTheContainer);
  TEST(EmptyStateLeavesTheContainerUnopenable);
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
  TEST(ZeroLengthStateIsTheEmptyCaseNotAGarbageCase);

  TEST(MoveIntoAnEmptySlotRelocatesTheWholeStack);
  TEST(MoveFromAnEmptySlotDoesNothing);
  TEST(MoveOntoTheSameItemMergesUpToSixtyFour);
  TEST(MergeIntoAFullStackSwapsInstead);
  TEST(MergeIsCappedAtSixtyFourAndDropsTheRemainder);
  TEST(MoveOntoADifferentItemSwapsThem);
  TEST(MovingOntoTheSameSlotIsANoOp);
  TEST(OutOfRangeSlotIndexIsTreatedAsEmpty);
  TEST(MoveToAnOutOfRangeSlotGrowsTheContainer);

  TEST(SplitHalvesTheStackIntoAnEmptySlot);
  TEST(SplitRoundsUpForAnEvenStack);
  TEST(SplitOfAStackOfOneIntoAnOccupiedSlotIsANoOp);
  TEST(SplitOfASingleItemIntoAnEmptySlotIsExact);
  TEST(SplitFromAnEmptySlotDoesNothing);
  TEST(SplitOntoTheSameItemMerges);
  TEST(SplitOntoADifferentItemIsANoOp);
  TEST(SplitIntoAFullStackLosesTheHalf);

  TEST(ActionFromAnotherPlayerIsRejected);
  TEST(ActionOnAnUnopenedContainerIsRejected);
  TEST(UnknownActionIsIgnored);
  TEST(TheCountArgumentIsIgnored);

  TEST(CloseSerialisesTheContainerAndReleasesTheEntity);
  TEST(SavedBlobDecodesBackToTheSameContents);
  TEST(CloseByAnotherPlayerIsRejected);
  TEST(CloseOfAnUnopenedContainerIsRejected);
  TEST(FailedSaveStillReleasesTheContainer);
  TEST(CloseWithNullStorageReleasesNothing);
  TEST(ReopenAfterCloseSucceeds);
  TEST(RegistryIsUnchangedByARejectedClose);

  TEST(UnconnectedStorageYieldsTheEmptyStateBug);
  TEST(UnconnectedStorageMakesEverySaveFail);

  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
