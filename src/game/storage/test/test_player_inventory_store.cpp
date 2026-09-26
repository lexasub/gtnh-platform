// PlayerInventoryStore unit tests (issue gp-obtt).
//
// File under test:
//   src/game/storage/PlayerInventoryStore.cpp — the in-memory player inventory
//   cache, and src/game/storage/PlayerInventoryStore.h for the PersistSlot
//   contract and kInventorySlots.
//
// WHAT IS PINNED
// --------------
// giveItem() is the single mutation entry point every producer funnels through
// (CraftRequestHandler, BreakBlockHandler's dry-run copy, GameScenario). Its
// three-pass allocation order is the contract the Go integration helper
// WaitForInventoryItem depends on (gp-b2r had to SUM counts across slots because
// one grant can land in several slots):
//
//   pass 0  target_slot (only when 0 <= target_slot < kInventorySlots):
//           an empty slot is FILLED, a same-item stack is topped up, and any
//           leftover spills into the general passes below,
//   pass 1  top up every existing same-item stack that is not already full,
//           scanning slots 0..39 in ascending order,
//   pass 2  fill empty slots, again in ascending order, at most kMaxStack each.
//
// Plus the invariants a corrupted cache would break: the total item count is
// always right, a rejected grant leaves every byte of every slot unchanged, and
// the change callbacks fire in slot order.
//
// OBSERVED, NOT BLESSED — see the individual cases
// ------------------------------------------------
// Three behaviours below contradict what a reader would expect and are asserted
// AS OBSERVED so a future fix shows up as a deliberate test change. Each names
// the exact line and the beads issue filed against it:
//   * a targeted grant larger than kMaxStack writes ONE over-full stack and
//     returns success (PlayerInventoryStore.cpp:94-96, gp-obtt-fix-1),
//   * a zero-count targeted grant creates a phantom occupied slot
//     (PlayerInventoryStore.cpp:94-96, gp-obtt-fix-1),
//   * setSlots / setSlotsAndCursor never emit the documented slot == 0xFFFF
//     full-replace sentinel (PlayerInventoryStore.h:25,58, gp-obtt-fix-2).
//
// DETERMINISM
// -----------
// In-memory state only, one fixed player id, no router client, no file, no
// clock, no sleeps, no network. Every assertion reads committed store state.
//
// Uses the PROJECT's own CHECK/TEST harness (src/engine/net/test/test.h
// convention, mirrored by src/game/machines/test/test_explosion_system.cpp).
// GoogleTest is deliberately NOT used: it is absent from conanfile.txt, CI does
// not install libgtest-dev, and CI builds Release with a global -Werror.

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "core_generated.h"

#include <game/storage/PlayerInventoryStore.h>

// ---------------------------------------------------------------------------
// Harness — definitions and macros near the TOP, before first use.
// ---------------------------------------------------------------------------

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

#define CHECK(cond, ...) \
  test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) \
  test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) \
  test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define TEST(name) \
  do {               \
    printf("  TEST: %s\n", #name); \
    test_##name();   \
  } while (0)

namespace {

using Slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>;

// Cast helper so CHECK_EQ never compares a signed and an unsigned operand
// (which -Wextra/-Werror would reject at the macro expansion site).
constexpr int i_(size_t v) { return static_cast<int>(v); }
constexpr int i_(int v) { return v; }
constexpr int i_(long long v) { return static_cast<int>(v); }
constexpr int i_(uint8_t v) { return static_cast<int>(v); }
constexpr int i_(uint16_t v) { return static_cast<int>(v); }
constexpr int i_(uint32_t v) { return static_cast<int>(v); }

// ---------------------------------------------------------------------------
// Named constants. The issue asks that every constant be asserted BY NAME so a
// change to it is visible; kMaxStack is a function-local constexpr in
// PlayerInventoryStore.cpp:84 (not exported by the header), so it is re-declared
// here with the same value and pinned by the cases below.
// ---------------------------------------------------------------------------
constexpr int kSlots = simcore::kInventorySlots;
constexpr uint8_t kMaxStack = 64;  // PlayerInventoryStore.cpp:84
constexpr uint64_t kPlayer = 1;    // single-player semantics (gp-qxbu)
constexpr uint64_t kOtherPlayer = 2;
constexpr uint16_t kItemA = 1000;
constexpr uint16_t kItemB = 1001;
constexpr uint16_t kItemC = 1002;
// The "no target slot" sentinel every production caller passes.
constexpr int32_t kNoTarget = -1;

simcore::PersistSlot item(uint16_t id, uint8_t count, uint16_t meta = 0) {
  return simcore::PersistSlot{id, count, meta};
}

bool sameSlot(const simcore::PersistSlot& a, const simcore::PersistSlot& b) {
  return a.item_id == b.item_id && a.count == b.count && a.meta == b.meta;
}

bool sameSlots(const Slots& a, const Slots& b) {
  for (size_t k = 0; k < a.size(); ++k) {
    if (!sameSlot(a[k], b[k])) return false;
  }
  return true;
}

// Total units of `id` held across all 40 slots — the quantity a caller must sum
// (why gp-b2r's WaitForInventoryItem sums instead of reading one slot).
int totalOf(const Slots& s, uint16_t id) {
  int n = 0;
  for (const auto& sl : s) {
    if (sl.item_id == id) n += sl.count;
  }
  return n;
}

int occupiedSlots(const Slots& s) {
  int n = 0;
  for (const auto& sl : s) {
    if (sl.item_id != 0) ++n;
  }
  return n;
}

// One recorded onChange_ callback.
struct Change {
  uint64_t player = 0;
  uint16_t slot = 0;
  uint16_t item_id = 0;
  uint8_t count = 0;
  uint16_t meta = 0;
};

// A store plus the transcript of both callbacks, so a case can assert the
// publish contract as well as the committed state.
struct Rig {
  simcore::PlayerInventoryStore store;
  std::vector<Change> changes;
  std::vector<uint64_t> mutations;

  Rig() {
    store.setOnChange([this](uint64_t player, uint16_t slot, uint16_t item_id,
                             uint8_t count, uint16_t meta) {
      changes.push_back(Change{player, slot, item_id, count, meta});
    });
    store.setPostMutation([this](uint64_t player, const Slots&) {
      mutations.push_back(player);
    });
  }

  void clearTranscript() {
    changes.clear();
    mutations.clear();
  }
};

} // namespace

// ---------------------------------------------------------------------------
// The two constants the whole module is sized by
// ---------------------------------------------------------------------------
static void test_inventory_slot_count_is_forty() {
  // kInventorySlots is what the client renders, what InventoryLoadHandler
  // publishes and what InventoryActionHandler range-checks against
  // (test_inventory_action_handler.cpp:623 pins the same 40 from the other
  // side). Asserted by name so a resize is a visible failure, not a silent
  // one-sided mismatch.
  CHECK_EQ(simcore::kInventorySlots, 40, "a player inventory is 40 slots");
  CHECK_EQ(i_(kSlots), 40, "the cached copy used by these cases is 40 too");
  CHECK_EQ(i_(kMaxStack), 64, "a stack holds 64 items (PlayerInventoryStore.cpp:84)");

  Rig r;
  // A default-constructed PersistSlot is the empty slot: item 0, count 0.
  const auto empty = simcore::PersistSlot{};
  CHECK_EQ(i_(empty.item_id), 0, "a default slot holds item 0");
  CHECK_EQ(i_(empty.count), 0, "a default slot has count 0");
  CHECK_EQ(i_(empty.meta), 0, "a default slot has meta 0");
  const Slots none = r.store.getSlots(kPlayer);
  CHECK(occupiedSlots(none) == 0, "an unknown player reads as 40 empty slots");
  CHECK(sameSlots(none, Slots{}), "an unknown player reads as the zeroed array");
}

// ---------------------------------------------------------------------------
// giveItem into an empty inventory — slot 0, full count
// ---------------------------------------------------------------------------
static void test_give_into_empty_inventory_fills_the_first_slot() {
  Rig r;
  r.store.initPlayer(kPlayer);

  CHECK(r.store.giveItem(kPlayer, kItemA, 5, kNoTarget),
        "a 5-item grant into an empty inventory succeeds");
  const Slots s = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(s[0].item_id), i_(kItemA), "item A lands in slot 0");
  CHECK_EQ(i_(s[0].count), 5, "with the whole count");
  CHECK_EQ(i_(s[0].meta), 0, "and meta 0 (giveItem never sets meta)");
  CHECK(occupiedSlots(s) == 1, "exactly one slot is occupied");

  // A second, DIFFERENT item cannot stack onto slot 0, so it takes slot 1.
  CHECK(r.store.giveItem(kPlayer, kItemB, 3, kNoTarget),
        "a different item also fits");
  const Slots s2 = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(s2[0].item_id), i_(kItemA), "slot 0 is untouched");
  CHECK_EQ(i_(s2[0].count), 5, "slot 0 keeps its count");
  CHECK_EQ(i_(s2[1].item_id), i_(kItemB), "item B lands in slot 1");
  CHECK_EQ(i_(s2[1].count), 3, "with the whole count");
  CHECK(occupiedSlots(s2) == 2, "exactly two slots are occupied");
}

// ---------------------------------------------------------------------------
// Topping an existing partial stack — pass 1 of the allocation order
// ---------------------------------------------------------------------------
static void test_grant_tops_up_an_existing_partial_stack_first() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 60);
  s[1] = item(kItemB, 2);
  r.store.setSlots(kPlayer, s);

  CHECK(r.store.giveItem(kPlayer, kItemA, 10, kNoTarget),
        "a 10-item grant tops up the partial stack and succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].item_id), i_(kItemA), "slot 0 still holds item A");
  CHECK_EQ(i_(after[0].count), 64, "slot 0 is filled to kMaxStack");
  CHECK_EQ(i_(after[1].item_id), i_(kItemB), "the unrelated slot 1 is untouched");
  CHECK_EQ(i_(after[1].count), 2, "the unrelated slot 1 keeps its count");
  // Pass 1 could only absorb 4 of the 10 (64 - 60), so pass 2 opens the first
  // empty slot for the remaining 6. The total is still exact.
  CHECK_EQ(i_(after[2].item_id), i_(kItemA), "the 6 leftover items opened slot 2");
  CHECK_EQ(i_(after[2].count), 6, "with the 10 - 4 that did not fit in slot 0");
  CHECK_EQ(totalOf(after, kItemA), 64 + 6, "the total is the original 60 plus 10");
  CHECK(occupiedSlots(after) == 3, "exactly one new slot was opened");
}

// A stack that is already full is skipped by pass 1, and the overflow spills
// into the next partial stack of the same item before any empty slot.
static void test_full_stacks_are_skipped_and_the_spill_goes_to_the_next() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 64);  // full
  s[1] = item(kItemA, 30);  // partial
  s[2] = item(kItemA, 64);  // full, sits BETWEEN the partial and the empties
  r.store.setSlots(kPlayer, s);

  // 40 items: pass 1 tops the partial slot 1 (30 -> 64, using 34) and skips
  // both full stacks; the 6 that are left over then take the first EMPTY slot,
  // which is slot 3, because pass 2 scans 0..39 and 0-2 are all occupied.
  CHECK(r.store.giveItem(kPlayer, kItemA, 40, kNoTarget), "the grant succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].count), 64, "the full slot 0 was skipped");
  CHECK_EQ(i_(after[1].count), 64, "the partial slot 1 was topped up to full");
  CHECK_EQ(i_(after[2].count), 64, "the full slot 2 was skipped");
  CHECK_EQ(i_(after[3].item_id), i_(kItemA), "the remainder opened slot 3");
  CHECK_EQ(i_(after[3].count), 6, "with the 6 that would not fit in slot 1");
  CHECK_EQ(totalOf(after, kItemA), 64 + 64 + 64 + 6,
           "the total is the 158 held plus the 40 granted");
  CHECK(occupiedSlots(after) == 4, "only one new slot was opened");
}

// A slot holding the item with count 0 is treated as a PARTIAL stack by pass 1
// (the predicate is item_id == id && count < kMaxStack), not as an empty slot,
// so it fills to kMaxStack instead of being reset with a fresh 64.
static void test_a_zero_count_slot_of_the_same_item_is_a_partial_stack() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 0);  // item set, count zero
  r.store.setSlots(kPlayer, s);

  CHECK(r.store.giveItem(kPlayer, kItemA, 3, kNoTarget), "the grant succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].count), 3, "slot 0 was topped up in place, not re-created");
  CHECK_EQ(totalOf(after, kItemA), 3, "the total is 3");
}

// A slot with item_id 0 and a non-zero count reads as EMPTY to pass 2
// (the predicate is item_id == 0), so it is overwritten wholesale — the stale
// count goes away with the slot.
static void test_a_count_with_item_zero_reads_as_an_empty_slot() {
  Rig r;
  Slots s{};
  s[0] = item(0, 7, 1234);  // item 0 means "no item", whatever count says
  r.store.setSlots(kPlayer, s);

  CHECK(r.store.giveItem(kPlayer, kItemA, 2, kNoTarget), "the grant succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].item_id), i_(kItemA), "slot 0 was reused");
  CHECK_EQ(i_(after[0].count), 2, "with the granted count");
  CHECK_EQ(i_(after[0].meta), 0, "and giveItem's meta 0, not the stale meta");
}

// ---------------------------------------------------------------------------
// Splitting a grant larger than one stack — pass 2
// ---------------------------------------------------------------------------
static void test_a_grant_larger_than_one_stack_splits_across_slots() {
  Rig r;
  r.store.initPlayer(kPlayer);

  // 100 = 64 + 36, so the total must survive the split and the caller must sum
  // across slots (this is why gp-b2r's WaitForInventoryItem sums).
  CHECK(r.store.giveItem(kPlayer, kItemA, 100, kNoTarget),
        "a 100-item grant into 40 free slots succeeds");
  const Slots s = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(s[0].item_id), i_(kItemA), "slot 0 is the first stack");
  CHECK_EQ(i_(s[0].count), 64, "capped at kMaxStack");
  CHECK_EQ(i_(s[1].item_id), i_(kItemA), "slot 1 carries the remainder");
  CHECK_EQ(i_(s[1].count), 36, "of 100 - 64");
  CHECK_EQ(i_(s[2].item_id), 0, "no third stack was opened");
  CHECK_EQ(totalOf(s, kItemA), 100, "the total count is exactly the grant");
  CHECK(occupiedSlots(s) == 2, "exactly two slots hold the grant");
}

static void test_a_grant_spreads_over_four_stacks_in_slot_order() {
  Rig r;
  r.store.initPlayer(kPlayer);

  // 255 is the largest uint8_t count, so it is the worst case for a 40-slot
  // inventory: 64 + 64 + 64 + 63.
  CHECK(r.store.giveItem(kPlayer, kItemA, 255, kNoTarget),
        "a 255-item grant into 40 free slots succeeds");
  const Slots s = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(s[0].count), 64, "stack 0 is full");
  CHECK_EQ(i_(s[1].count), 64, "stack 1 is full");
  CHECK_EQ(i_(s[2].count), 64, "stack 2 is full");
  CHECK_EQ(i_(s[3].count), 63, "stack 3 holds the remainder");
  CHECK_EQ(i_(s[4].item_id), 0, "nothing spilled into slot 4");
  CHECK_EQ(totalOf(s, kItemA), 255, "the total is exactly 255");
  CHECK(occupiedSlots(s) == 4, "exactly four stacks were opened");
}

static void test_a_zero_count_grant_without_a_target_changes_nothing() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 3);
  r.store.setSlots(kPlayer, s);
  r.clearTranscript();

  CHECK(r.store.giveItem(kPlayer, kItemA, 0, kNoTarget),
        "granting zero items reports success");
  CHECK(sameSlots(r.store.getSlots(kPlayer), s),
        "and leaves every slot byte-identical");
  CHECK(r.changes.empty() == false,
        "but still fires the per-slot change callbacks for all 40 slots");
  CHECK_EQ(i_(r.changes.size()), 40, "one callback per slot");
}

// ---------------------------------------------------------------------------
// The target_slot fast path
// ---------------------------------------------------------------------------
static void test_target_slot_tops_up_a_matching_stack() {
  Rig r;
  Slots s{};
  s[3] = item(kItemA, 10);
  s[4] = item(kItemA, 5);
  r.store.setSlots(kPlayer, s);

  // 20 into slot 3: it already holds 10 and kMaxStack is 64, so ALL 20 fit in
  // the target and the general passes never run — slot 4 keeps its 5.
  CHECK(r.store.giveItem(kPlayer, kItemA, 20, 3), "the targeted grant succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[3].count), 30, "slot 3 took the whole 20 (10 + 20 < 64)");
  CHECK_EQ(i_(after[4].count), 5, "the other same-item stack was left alone");
  CHECK_EQ(totalOf(after, kItemA), 10 + 5 + 20, "the total is 35");
  CHECK(occupiedSlots(after) == 2, "no new slot was opened");

  // A targeted grant that DOES overflow the target spills onward: 60 into slot 3
  // fills it to 64 and the 6 leftover go to the next same-item partial stack.
  r.store.setSlots(kPlayer, s);
  CHECK(r.store.giveItem(kPlayer, kItemA, 60, 3), "the overflowing grant succeeds");
  const Slots spill = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(spill[3].count), 64, "slot 3 was filled to kMaxStack");
  CHECK_EQ(i_(spill[4].count), 11, "the 6-item remainder topped up slot 4");
  CHECK_EQ(totalOf(spill, kItemA), 64 + 11, "the total is 10 + 5 + 60");
}

static void test_target_slot_occupies_an_empty_slot_and_ignores_earlier_room() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 4);  // room here, but slot 7 was asked for
  r.store.setSlots(kPlayer, s);

  CHECK(r.store.giveItem(kPlayer, kItemA, 3, 7), "the targeted grant succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].count), 4, "the earlier partial stack is untouched");
  CHECK_EQ(i_(after[7].item_id), i_(kItemA), "slot 7 received the items");
  CHECK_EQ(i_(after[7].count), 3, "with the whole count");
  CHECK_EQ(totalOf(after, kItemA), 7, "the total is 4 + 3");
}

static void test_target_slot_holding_another_item_is_never_overwritten() {
  Rig r;
  Slots s{};
  s[2] = item(kItemB, 8);
  r.store.setSlots(kPlayer, s);

  // Neither branch of the target_slot block matches (slot is occupied by a
  // DIFFERENT item), so the target is skipped entirely and the general passes
  // place the items. Asserted as observed: giveItem never merges two item ids
  // and never discards the existing stack.
  CHECK(r.store.giveItem(kPlayer, kItemA, 4, 2), "the grant succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[2].item_id), i_(kItemB), "the occupied target slot is intact");
  CHECK_EQ(i_(after[2].count), 8, "and keeps its count");
  CHECK_EQ(i_(after[0].item_id), i_(kItemA), "the items went to the first free slot");
  CHECK_EQ(i_(after[0].count), 4, "with the whole count");
  CHECK_EQ(totalOf(after, kItemA), 4, "the total is 4");
  CHECK_EQ(totalOf(after, kItemB), 8, "item B is untouched");
}

static void test_an_out_of_range_target_slot_is_ignored_entirely() {
  Rig r;
  r.store.initPlayer(kPlayer);

  // kInventorySlots is out of range, so the target fast path is skipped and the
  // general passes place the items from slot 0.
  CHECK(r.store.giveItem(kPlayer, kItemA, 2, kSlots), "a target of kInventorySlots still succeeds");
  const Slots atEnd = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(atEnd[0].item_id), i_(kItemA), "the items landed in slot 0");
  CHECK_EQ(i_(atEnd[0].count), 2, "with the whole count");
  CHECK_EQ(occupiedSlots(atEnd), 1, "and no slot beyond the end was written");

  // A second, well past the end, and a negative sentinel other than -1.
  CHECK(r.store.giveItem(kPlayer, kItemB, 1, 100000), "a far out-of-range target still succeeds");
  CHECK(r.store.giveItem(kPlayer, kItemC, 1, -2), "a target below -1 still succeeds");
  const Slots all = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(all[0].item_id), i_(kItemA), "slot 0 still holds item A");
  CHECK_EQ(i_(all[0].count), 2, "with its count");
  CHECK_EQ(i_(all[1].item_id), i_(kItemB), "item B took slot 1");
  CHECK_EQ(i_(all[2].item_id), i_(kItemC), "item C took slot 2");
  CHECK_EQ(occupiedSlots(all), 3, "each out-of-range target was simply ignored");
}

// OBSERVED, NOT BLESSED: the target_slot fast path (PlayerInventoryStore.cpp:94-96)
// writes the WHOLE grant into an empty target slot with no kMaxStack clamp, so a
// targeted grant above 64 produces a single over-full stack and still reports
// success. Every other path clamps at kMaxStack. Filed as gp-obtt-fix-1.
static void test_a_targeted_grant_above_max_stack_writes_one_overfull_stack() {
  Rig r;
  r.store.initPlayer(kPlayer);

  const bool ok = r.store.giveItem(kPlayer, kItemA, 100, 5);
  const Slots s = r.store.getSlots(kPlayer);

  CHECK(ok, "OBSERVED: an over-sized targeted grant still reports success");
  CHECK_EQ(i_(s[5].item_id), i_(kItemA), "the items are in the target slot");
  CHECK_EQ(i_(s[5].count), 100,
           "OBSERVED: one stack of 100 — the target path does not clamp to kMaxStack");
  CHECK(i_(s[5].count) > i_(kMaxStack),
        "the stack is over-full, which no other giveItem path can produce");
  CHECK(occupiedSlots(s) == 1, "the split passes never ran");
  CHECK_EQ(totalOf(s, kItemA), 100,
           "the total is still correct, so a caller summing counts is not misled");
}

// OBSERVED, NOT BLESSED: the same branch with count 0 stores {item_id, 0} in a
// slot that was empty, which every other "is this slot free?" predicate in the
// codebase reads as OCCUPIED. Filed as gp-obtt-fix-1.
static void test_a_zero_count_targeted_grant_creates_a_phantom_slot() {
  Rig r;
  r.store.initPlayer(kPlayer);

  CHECK(r.store.giveItem(kPlayer, kItemA, 0, 4), "a zero-count grant reports success");
  const Slots s = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(s[4].item_id), i_(kItemA),
           "OBSERVED: the target slot now names item A");
  CHECK_EQ(i_(s[4].count), 0, "OBSERVED: with a count of zero");
  CHECK(occupiedSlots(s) == 1,
        "OBSERVED: a consumer summing item_id != 0 counts this phantom as occupied");

  // And it is not harmless: the phantom now absorbs the next grant through the
  // partial-stack pass, because a slot with count 0 and a matching item id has
  // a full 64 items of "room".
  CHECK(r.store.giveItem(kPlayer, kItemA, 5, kNoTarget), "the follow-up grant succeeds");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[4].count), 5, "the phantom slot absorbed all 5 items");
  CHECK(occupiedSlots(after) == 1, "no new slot was opened");
}

// ---------------------------------------------------------------------------
// A full inventory: the grant fails and state is untouched
// ---------------------------------------------------------------------------
static void test_a_grant_into_a_full_inventory_fails_without_corrupting_state() {
  Rig r;
  // 40 distinct items, 64 of each: no empty slot, no same-item room.
  Slots s{};
  for (int i = 0; i < kSlots; ++i) {
    s[static_cast<size_t>(i)] = item(static_cast<uint16_t>(2000 + i), 64);
  }
  r.store.setSlots(kPlayer, s);
  r.clearTranscript();

  const bool ok = r.store.giveItem(kPlayer, kItemA, 1, kNoTarget);
  CHECK(!ok, "a grant into a full inventory FAILS (returns false)");
  CHECK(sameSlots(r.store.getSlots(kPlayer), s),
        "every one of the 40 slots is byte-identical afterwards");
  CHECK_EQ(occupiedSlots(r.store.getSlots(kPlayer)), 40, "still 40 occupied slots");

  // The failure is reported only by the return value and the log line — the
  // change callbacks still fire for the whole (unchanged) array, so a caller
  // that persists from onChange re-persists the same values. Pinned because the
  // integration helper watches for exactly this.
  CHECK_EQ(i_(r.changes.size()), 40, "onChange still fires once per slot");
  CHECK_EQ(i_(r.mutations.size()), 1, "postMutation still fires exactly once");
  bool unchangedInTranscript = true;
  for (const auto& c : r.changes) {
    const auto& sl = s[c.slot];
    if (c.item_id != sl.item_id || c.count != sl.count || c.meta != sl.meta) {
      unchangedInTranscript = false;
    }
  }
  CHECK(unchangedInTranscript, "and every reported value equals the stored value");
}

static void test_a_partial_grant_into_an_almost_full_inventory_succeeds_exactly() {
  Rig r;
  // 39 full stacks, one free slot. Granting 70 must place 64 in the free slot
  // and drop the remaining 6, returning false even though 64 of 70 were placed.
  Slots s{};
  for (int i = 0; i < kSlots - 1; ++i) {
    s[static_cast<size_t>(i)] = item(static_cast<uint16_t>(3000 + i), 64);
  }
  r.store.setSlots(kPlayer, s);

  const bool ok = r.store.giveItem(kPlayer, kItemA, 70, kNoTarget);
  const Slots after = r.store.getSlots(kPlayer);
  CHECK(!ok, "OBSERVED: a partially-satisfiable grant still returns false");
  CHECK_EQ(i_(after[kSlots - 1].item_id), i_(kItemA), "the one free slot was used");
  CHECK_EQ(i_(after[kSlots - 1].count), 64, "and filled to kMaxStack");
  CHECK_EQ(occupiedSlots(after), 40, "the inventory is now full");
  CHECK_EQ(totalOf(after, kItemA), 64,
           "64 of the 70 were placed and the 6 that did not fit are gone");

  // A second grant now has nowhere to go and changes nothing.
  r.clearTranscript();
  CHECK(!r.store.giveItem(kPlayer, kItemA, 1, kNoTarget),
        "the next grant into the now-full inventory fails");
  CHECK(sameSlots(r.store.getSlots(kPlayer), after),
        "and leaves the full inventory byte-identical");
}

static void test_a_full_inventory_does_not_block_a_same_item_top_up() {
  Rig r;
  Slots s{};
  for (int i = 0; i < kSlots; ++i) {
    s[static_cast<size_t>(i)] =
        (i == 20) ? item(kItemA, 10) : item(static_cast<uint16_t>(4000 + i), 64);
  }
  r.store.setSlots(kPlayer, s);

  // A full inventory is not a full stack: the one partial stack of the same item
  // still absorbs the grant.
  CHECK(r.store.giveItem(kPlayer, kItemA, 5, kNoTarget),
        "a top-up succeeds even though no slot is empty");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[20].count), 15, "the partial stack absorbed the 5 items");
  CHECK_EQ(occupiedSlots(after), 40, "no new slot was needed");
  CHECK_EQ(totalOf(after, kItemA), 15, "the total is 10 + 5");
}

// ---------------------------------------------------------------------------
// The change-callback contract (what MetaDB persists and the client repaints)
// ---------------------------------------------------------------------------
static void test_change_callbacks_fire_once_per_slot_in_ascending_order() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 2);
  s[39] = item(kItemB, 3);
  r.store.setSlots(kPlayer, s);
  r.clearTranscript();

  r.store.giveItem(kPlayer, kItemC, 1, kNoTarget);

  CHECK_EQ(i_(r.changes.size()), kSlots, "one callback per slot, not per change");
  bool ascending = true;
  bool payloadMatches = true;
  const Slots after = r.store.getSlots(kPlayer);
  for (size_t i = 0; i < r.changes.size(); ++i) {
    const auto& c = r.changes[i];
    if (c.slot != i) ascending = false;
    if (c.player != kPlayer) payloadMatches = false;
    if (c.item_id != after[i].item_id || c.count != after[i].count ||
        c.meta != after[i].meta) {
      payloadMatches = false;
    }
  }
  CHECK(ascending, "the slots are reported 0..39 in ascending order");
  CHECK(payloadMatches, "each callback carries that slot's committed value");
  CHECK_EQ(i_(r.mutations.size()), 1, "postMutation fires exactly once per grant");
  CHECK_EQ(i_(r.mutations[0]), i_(kPlayer), "for the mutated player");
}

// OBSERVED, NOT BLESSED: PlayerInventoryStore.h:25 and :58 document an extra
// callback with slot_index == 0xFFFF carrying the whole array for a full
// replace. No code path emits it — setSlots fires exactly 40 per-slot callbacks
// and one postMutation. Filed as gp-obtt-fix-2.
static void test_no_full_replace_sentinel_is_ever_published() {
  constexpr uint16_t kFullReplaceSentinel = 0xFFFF;  // PlayerInventoryStore.h:25

  {
    Rig r;
    Slots s{};
    s[0] = item(kItemA, 1);
    r.store.setSlots(kPlayer, s);
    int sentinels = 0;
    for (const auto& c : r.changes) {
      if (c.slot == kFullReplaceSentinel) ++sentinels;
    }
    CHECK_EQ(sentinels, 0, "OBSERVED: setSlots publishes no 0xFFFF sentinel");
    CHECK_EQ(i_(r.changes.size()), 40, "OBSERVED: only the 40 per-slot callbacks");
    CHECK_EQ(i_(r.mutations.size()), 1, "the snapshot is the postMutation publish");
  }
  {
    Rig r;
    Slots s{};
    s[0] = item(kItemA, 1);
    r.store.setSlotsAndCursor(kPlayer, s, item(kItemB, 2));
    int sentinels = 0;
    for (const auto& c : r.changes) {
      if (c.slot == kFullReplaceSentinel) ++sentinels;
    }
    CHECK_EQ(sentinels, 0, "OBSERVED: setSlotsAndCursor publishes no 0xFFFF sentinel");
    CHECK_EQ(i_(r.changes.size()), 40, "OBSERVED: only the 40 per-slot callbacks");
    CHECK_EQ(i_(r.mutations.size()), 1, "and exactly ONE snapshot for both writes");
  }
  {
    Rig r;
    r.store.initPlayer(kPlayer);
    r.store.giveItem(kPlayer, kItemA, 1, kNoTarget);
    int sentinels = 0;
    for (const auto& c : r.changes) {
      if (c.slot == kFullReplaceSentinel) ++sentinels;
    }
    CHECK_EQ(sentinels, 0, "OBSERVED: giveItem publishes no 0xFFFF sentinel");
  }
}

// ---------------------------------------------------------------------------
// setSlots / setCursor / setSlotsAndCursor
// ---------------------------------------------------------------------------
static void test_setSlots_reports_every_slot_even_the_unchanged_ones() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 1);
  s[1] = item(kItemB, 2);
  r.store.setSlots(kPlayer, s);
  r.clearTranscript();

  // Rewriting the identical array is still a full publish.
  r.store.setSlots(kPlayer, s);
  CHECK_EQ(i_(r.changes.size()), 40, "all 40 slots are reported, not just the diff");
  CHECK_EQ(i_(r.mutations.size()), 1, "one snapshot");
  CHECK_EQ(i_(r.changes[0].item_id), i_(kItemA), "slot 0 is reported first");
  CHECK_EQ(i_(r.changes[1].item_id), i_(kItemB), "then slot 1");
  CHECK_EQ(i_(r.changes[2].item_id), 0, "and the empty ones");
}

static void test_set_cursor_fires_no_change_callback() {
  Rig r;
  r.store.initPlayer(kPlayer);
  r.clearTranscript();

  r.store.setCursor(kPlayer, item(kItemA, 9, 4));
  CHECK(r.changes.empty(),
        "the cursor is not an inventory slot, so onChange does not fire");
  CHECK_EQ(i_(r.mutations.size()), 1, "but the snapshot still publishes");

  const auto cur = r.store.getCursor(kPlayer);
  CHECK_EQ(i_(cur.item_id), i_(kItemA), "the cursor keeps the item");
  CHECK_EQ(i_(cur.count), 9, "and the count");
  CHECK_EQ(i_(cur.meta), 4, "and the meta");

  // The cursor is per player and defaults to empty.
  const auto other = r.store.getCursor(kOtherPlayer);
  CHECK_EQ(i_(other.item_id), 0, "another player's cursor is empty");
  CHECK_EQ(i_(other.count), 0, "with a zero count");
  r.store.setCursor(kOtherPlayer, item(kItemB, 1));
  CHECK_EQ(i_(r.store.getCursor(kPlayer).item_id), i_(kItemA),
           "the two cursors are independent");
}

static void test_set_slots_and_cursor_publishes_one_snapshot() {
  Rig r;
  r.store.initPlayer(kPlayer);
  r.clearTranscript();

  Slots s{};
  s[0] = item(kItemA, 3);
  r.store.setSlotsAndCursor(kPlayer, s, item(kItemB, 7));

  CHECK_EQ(i_(r.changes.size()), 40, "one callback per slot");
  CHECK_EQ(i_(r.mutations.size()), 1,
           "EXACTLY ONE snapshot even though slots and cursor both changed");
  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].count), 3, "the slots landed");
  CHECK_EQ(i_(r.store.getCursor(kPlayer).count), 7, "the cursor landed");
}

// ---------------------------------------------------------------------------
// initPlayer / applyUpdate
// ---------------------------------------------------------------------------
static void test_init_player_is_idempotent_and_never_clears_state() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 5);
  r.store.setSlots(kPlayer, s);

  // A second join for the same player must not wipe the cached inventory —
  // PlayerJoinedHandler::handle calls initPlayer on every join.
  r.store.initPlayer(kPlayer);
  r.store.initPlayer(kPlayer);
  CHECK(sameSlots(r.store.getSlots(kPlayer), s),
        "initPlayer on a known player leaves the inventory intact");
  CHECK_EQ(totalOf(r.store.getSlots(kPlayer), kItemA), 5, "5 items are still held");

  r.store.initPlayer(kOtherPlayer);
  const Slots fresh = r.store.getSlots(kOtherPlayer);
  CHECK(occupiedSlots(fresh) == 0, "a fresh player starts with an empty inventory");
  CHECK(sameSlots(r.store.getSlots(kPlayer), s), "and the other player is untouched");
}

static void test_apply_update_writes_a_prefix_and_never_publishes() {
  Rig r;
  Slots s{};
  for (int i = 0; i < kSlots; ++i) {
    s[static_cast<size_t>(i)] = item(static_cast<uint16_t>(5000 + i), 1);
  }
  r.store.setSlots(kPlayer, s);
  r.clearTranscript();

  // applyUpdate is the MetaDB restore path (InventoryLoadHandler): it overwrites
  // the leading slots and leaves the rest alone, with NO callback — the caller
  // publishes the snapshot itself.
  std::vector<simcore::PersistSlot> update;
  update.push_back(item(kItemA, 12));
  update.push_back(item(kItemB, 34));
  r.store.applyUpdate(kPlayer, update);

  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].item_id), i_(kItemA), "slot 0 was overwritten");
  CHECK_EQ(i_(after[0].count), 12, "with the restored count");
  CHECK_EQ(i_(after[1].item_id), i_(kItemB), "slot 1 was overwritten");
  CHECK_EQ(i_(after[1].count), 34, "with the restored count");
  CHECK_EQ(i_(after[2].item_id), 5002, "slot 2 was NOT touched");
  CHECK_EQ(i_(after[39].item_id), 5039, "slot 39 was NOT touched");
  CHECK(r.changes.empty(), "applyUpdate publishes no change callback");
  CHECK(r.mutations.empty(), "and no snapshot — the caller owns the publish");

  // An empty update vector changes nothing at all, and no state is created.
  r.store.applyUpdate(kPlayer, {});
  CHECK(sameSlots(r.store.getSlots(kPlayer), after),
        "an empty update is a no-op on every slot");
}

static void test_apply_update_truncates_an_oversized_vector() {
  Rig r;
  r.store.initPlayer(kPlayer);

  std::vector<simcore::PersistSlot> update;
  for (size_t i = 0; i < kSlots + 5; ++i) {
    update.push_back(item(static_cast<uint16_t>(6000 + i),
                          static_cast<uint8_t>(i % 255)));
  }
  r.store.applyUpdate(kPlayer, update);

  const Slots after = r.store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].item_id), 6000, "the first of the extra slots landed");
  CHECK_EQ(i_(after[kSlots - 1].item_id), static_cast<int>(6000 + kSlots - 1),
           "the 40th slot holds the 40th update entry");
  CHECK(occupiedSlots(after) == kSlots,
        "the 5 surplus entries were dropped, not written past the end");
}

static void test_apply_update_creates_the_player_entry_when_absent() {
  Rig r;
  CHECK(occupiedSlots(r.store.getSlots(kOtherPlayer)) == 0, "player 2 is unknown");
  r.store.applyUpdate(kOtherPlayer, {item(kItemA, 3)});
  const Slots after = r.store.getSlots(kOtherPlayer);
  CHECK_EQ(i_(after[0].count), 3, "the update landed on an unknown player");
  CHECK(occupiedSlots(r.store.getSlots(kPlayer)) == 0,
        "and did not touch the other player");
}

// ---------------------------------------------------------------------------
// buildUpdate — the FlatBuffer InventoryLoadHandler republishes
// ---------------------------------------------------------------------------
static void test_build_update_publishes_forty_slots_and_the_cursor() {
  Rig r;
  Slots s{};
  s[0] = item(kItemA, 11, 3);
  s[39] = item(kItemB, 22, 4);
  r.store.setSlots(kPlayer, s);
  r.store.setCursor(kPlayer, item(kItemC, 5, 6));

  flatbuffers::FlatBufferBuilder fb(512);
  auto off = r.store.buildUpdate(fb, kPlayer);
  fb.Finish(off);
  const auto* up = flatbuffers::GetRoot<Protocol::InventoryUpdate>(fb.GetBufferPointer());

  CHECK_EQ(up->player_id(), kPlayer, "the update carries the player id");
  const auto* slots = up->slots();
  CHECK(slots != nullptr, "the slot vector is present");
  if (slots) {
    CHECK_EQ(i_(slots->size()), kSlots, "all 40 slots are published, empties included");
    CHECK_EQ(i_(slots->Get(0)->item_id()), i_(kItemA), "slot 0 carries the item");
    CHECK_EQ(i_(slots->Get(0)->count()), 11, "and the count");
    CHECK_EQ(i_(slots->Get(0)->meta()), 3, "and the meta");
    CHECK_EQ(i_(slots->Get(1)->item_id()), 0, "an empty slot is published as item 0");
    CHECK_EQ(i_(slots->Get(39)->item_id()), i_(kItemB), "slot 39 carries the item");
    CHECK_EQ(i_(slots->Get(39)->count()), 22, "and the count");
  }
  const auto* cur = up->cursor();
  CHECK(cur != nullptr, "the server-owned cursor is present");
  if (cur) {
    CHECK_EQ(i_(cur->item_id()), i_(kItemC), "the cursor carries the item");
    CHECK_EQ(i_(cur->count()), 5, "and the count");
    CHECK_EQ(i_(cur->meta()), 6, "and the meta");
  }
  CHECK_EQ(i_(up->container_id()), 0, "container_id 0 means no open container");
  const auto* pos = up->container_pos();
  CHECK(pos != nullptr, "a container position is always present");
  if (pos) {
    CHECK_EQ(i_(pos->x()), 0, "at the origin x");
    CHECK_EQ(i_(pos->y()), 0, "at the origin y");
    CHECK_EQ(i_(pos->z()), 0, "at the origin z");
  }
  CHECK(up->container_slots() != nullptr, "the container slot vector is present");
  if (up->container_slots()) {
    CHECK_EQ(i_(up->container_slots()->size()), 0, "and empty for a player-only snapshot");
  }
}

static void test_build_update_for_an_unknown_player_publishes_no_slots() {
  Rig r;
  flatbuffers::FlatBufferBuilder fb(512);
  auto off = r.store.buildUpdate(fb, 4242);
  fb.Finish(off);
  const auto* up = flatbuffers::GetRoot<Protocol::InventoryUpdate>(fb.GetBufferPointer());

  CHECK_EQ(up->player_id(), 4242u, "the update still carries the requested id");
  CHECK(up->slots() != nullptr, "the slot vector is present");
  if (up->slots()) {
    CHECK_EQ(i_(up->slots()->size()), 0,
             "OBSERVED: an unknown player publishes ZERO slots, not 40 empties");
  }
  const auto* cur = up->cursor();
  CHECK(cur != nullptr, "the cursor is present");
  if (cur) {
    CHECK_EQ(i_(cur->item_id()), 0, "with item 0 for an unknown player");
    CHECK_EQ(i_(cur->count()), 0, "and count 0");
  }
}

static void test_build_update_reflects_a_committed_grant() {
  Rig r;
  r.store.initPlayer(kPlayer);
  r.store.giveItem(kPlayer, kItemA, 100, kNoTarget);

  flatbuffers::FlatBufferBuilder fb(512);
  auto off = r.store.buildUpdate(fb, kPlayer);
  fb.Finish(off);
  const auto* up = flatbuffers::GetRoot<Protocol::InventoryUpdate>(fb.GetBufferPointer());
  const auto* slots = up->slots();
  CHECK(slots != nullptr, "the slot vector is present");
  if (slots) {
    CHECK_EQ(i_(slots->size()), kSlots, "40 slots are published");
    // The exact contract a client summing wire counts relies on: the split is
    // visible on the wire, 64 + 36.
    CHECK_EQ(i_(slots->Get(0)->count()), 64, "slot 0 holds 64 on the wire");
    CHECK_EQ(i_(slots->Get(1)->count()), 36, "slot 1 holds 36 on the wire");
  }
}

// ---------------------------------------------------------------------------
// Per-player isolation across the whole surface
// ---------------------------------------------------------------------------
static void test_two_players_never_share_a_slot() {
  Rig r;
  r.store.initPlayer(kPlayer);
  r.store.initPlayer(kOtherPlayer);
  r.store.giveItem(kPlayer, kItemA, 70, kNoTarget);
  r.store.giveItem(kOtherPlayer, kItemB, 1, kNoTarget);

  const Slots a = r.store.getSlots(kPlayer);
  const Slots b = r.store.getSlots(kOtherPlayer);
  CHECK_EQ(totalOf(a, kItemA), 70, "player 1 holds all 70 of item A");
  CHECK_EQ(totalOf(a, kItemB), 0, "player 1 holds no item B");
  CHECK_EQ(totalOf(b, kItemB), 1, "player 2 holds its single item B");
  CHECK_EQ(totalOf(b, kItemA), 0, "player 2 holds no item A");
  CHECK_EQ(occupiedSlots(a), 2, "player 1's grant split across 2 slots");
  CHECK_EQ(occupiedSlots(b), 1, "player 2's grant took 1 slot");

  // And the callbacks were addressed to the right player.
  bool sawA = false;
  bool sawB = false;
  for (const auto& c : r.changes) {
    if (c.player == kPlayer) sawA = true;
    if (c.player == kOtherPlayer) sawB = true;
  }
  CHECK(sawA, "player 1's grant was reported for player 1");
  CHECK(sawB, "player 2's grant was reported for player 2");
}

// ---------------------------------------------------------------------------
// GameMode — the last two lines of the class, keyed the same way
// ---------------------------------------------------------------------------
static void test_game_mode_is_per_player_and_defaults_to_zero() {
  Rig r;
  CHECK_EQ(i_(r.store.getGameMode(kPlayer)), 0, "an unknown player is in mode 0");
  r.store.setGameMode(kPlayer, 1);
  r.store.setGameMode(kOtherPlayer, 3);
  CHECK_EQ(i_(r.store.getGameMode(kPlayer)), 1, "player 1 kept its mode");
  CHECK_EQ(i_(r.store.getGameMode(kOtherPlayer)), 3, "player 2 kept its mode");
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("=== player_inventory_store test suite ===\n\n");

  TEST(inventory_slot_count_is_forty);
  TEST(give_into_empty_inventory_fills_the_first_slot);
  TEST(grant_tops_up_an_existing_partial_stack_first);
  TEST(full_stacks_are_skipped_and_the_spill_goes_to_the_next);
  TEST(a_zero_count_slot_of_the_same_item_is_a_partial_stack);
  TEST(a_count_with_item_zero_reads_as_an_empty_slot);
  TEST(a_grant_larger_than_one_stack_splits_across_slots);
  TEST(a_grant_spreads_over_four_stacks_in_slot_order);
  TEST(a_zero_count_grant_without_a_target_changes_nothing);
  TEST(target_slot_tops_up_a_matching_stack);
  TEST(target_slot_occupies_an_empty_slot_and_ignores_earlier_room);
  TEST(target_slot_holding_another_item_is_never_overwritten);
  TEST(an_out_of_range_target_slot_is_ignored_entirely);
  TEST(a_targeted_grant_above_max_stack_writes_one_overfull_stack);
  TEST(a_zero_count_targeted_grant_creates_a_phantom_slot);
  TEST(a_grant_into_a_full_inventory_fails_without_corrupting_state);
  TEST(a_partial_grant_into_an_almost_full_inventory_succeeds_exactly);
  TEST(a_full_inventory_does_not_block_a_same_item_top_up);
  TEST(change_callbacks_fire_once_per_slot_in_ascending_order);
  TEST(no_full_replace_sentinel_is_ever_published);
  TEST(setSlots_reports_every_slot_even_the_unchanged_ones);
  TEST(set_cursor_fires_no_change_callback);
  TEST(set_slots_and_cursor_publishes_one_snapshot);
  TEST(init_player_is_idempotent_and_never_clears_state);
  TEST(apply_update_writes_a_prefix_and_never_publishes);
  TEST(apply_update_truncates_an_oversized_vector);
  TEST(apply_update_creates_the_player_entry_when_absent);
  TEST(build_update_publishes_forty_slots_and_the_cursor);
  TEST(build_update_for_an_unknown_player_publishes_no_slots);
  TEST(build_update_reflects_a_committed_grant);
  TEST(two_players_never_share_a_slot);
  TEST(game_mode_is_per_player_and_defaults_to_zero);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
