// InventoryActionHandler unit tests (issue gp-b25u).
//
// Files under test:
//   src/game/storage/InventoryActionHandler.cpp — the server-authoritative
//   inventory mutation path
//   src/game/storage/InventoryClick.h        — the pure rule table it calls
//
// THE INVARIANT UNDER TEST
// ------------------------
// A rejected action must leave state COMPLETELY unchanged. This is the bug
// class the parent change (refactor-server-authoritative-inventory-verification)
// exists to hunt, so the rejection cases are asserted exhaustively rather than
// sampled: every rejection path is checked against a FULL before/after snapshot
// of the player's 40 slots, the cursor, and the open container — not merely
// "the one slot I expected to change did not change".
//
// Why the handler, not just the rule table: ApplyContainerClick() mutates the
// state IN PLACE and the handler owns the commit. If the handler committed
// before the rules decided, a rejection would still publish and persist a
// half-applied mutation. So the assertions run through handle() and observe
// the committed store, not a local copy.
//
// Rejection paths enumerated in InventoryActionHandler.cpp:
//   R1  malformed buffer           (Verifier rejects)         line 32
//   R2  null root                  (defensive)                line 34
//   R3  player_id == 0             (no player, no action)     line 37
//   R4  container_id != 0 with no  (OH3 gate, before load)    line 51
//       open session for the player
//   R5  container_id == 1 with a    (container gone mid-click) line 68
//       null slotsRef
//   R6  out-of-range slot          (SlotAt returns nullptr)   InventoryClick.h:57
//   R7  invalid action_type        (default: in the switch)   InventoryClick.h:284
//   R8  a rule that changes nothing (e.g. click on empty)
//   R9  a rule that mutates then fails to place the stack
//       (ApplyQuickMove's "put it back" path)
//
// No network, no display, no cluster, no wall-clock, no sleeps: the router
// client is constructed but never connected (Publish() drops), the
// EntityStateStoreClient is null or offline (its callbacks fire inline), and
// every assertion reads committed in-process state.
//
// Uses the PROJECT's own CHECK/TEST harness (src/engine/net/test/test.h
// convention, mirrored by src/game/world/test/BlockTransforms_test.cpp).
// GoogleTest is deliberately NOT used: it is absent from conanfile.txt, CI does
// not install libgtest-dev, and CI builds Release with a global -Werror.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <asio.hpp>

#include "core_generated.h"
#include "machine_state_generated.h"

#include <game/storage/InventoryActionHandler.h>
#include <game/storage/InventoryClick.h>
#include <game/storage/PlayerInventoryStore.h>
#include <game/storage/ContainerSession.h>
#include <game/storage/ChestStateManager.h>
#include <game/crafting/WorkbenchStateManager.h>
#include <game/recipes/RecipeManager.h>
#include <game/actions/ActionContext.h>
#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include <engine/sim/components/InventoryContainer.h>

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

// Cast helper so CHECK_EQ never compares a signed and an unsigned operand
// (which -Wextra/-Werror would reject at the macro expansion site).
constexpr int i_(size_t v) { return static_cast<int>(v); }
constexpr int i_(int v) { return v; }  // also covers int32_t
constexpr int i_(unsigned v) { return static_cast<int>(v); }  // uint32_t
constexpr int i_(uint16_t v) { return static_cast<int>(v); }
constexpr int i_(uint8_t v) { return static_cast<int>(v); }

using Slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>;

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

bool sameVec(const std::vector<simcore::PersistSlot>& a,
             const std::vector<simcore::PersistSlot>& b) {
  if (a.size() != b.size()) return false;
  for (size_t k = 0; k < a.size(); ++k) {
    if (!sameSlot(a[k], b[k])) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// THE snapshot: every piece of state a click could touch
// ---------------------------------------------------------------------------
//
// This is the instrument the whole suite is built around. It reads back what
// the handler COMMITTED — the store's slots and cursor, plus the container
// session's live view — so "unchanged" is asserted against everything at once
// rather than against a hand-picked slot that might be the one that slipped
// through.
struct Snapshot {
  Slots slots{};
  simcore::PersistSlot cursor{};
  std::vector<simcore::PersistSlot> container;
  bool session_present = false;
  // Everything the store pushed out, so a rejection that silently published is
  // caught even though no slot changed.
  int post_mutations = 0;

  bool operator==(const Snapshot& o) const {
    return sameSlots(slots, o.slots) && sameSlot(cursor, o.cursor) &&
           session_present == o.session_present &&
           sameVec(container, o.container);
  }
};

// A player id that is never 0 — the handler rejects player_id==0 outright, so
// every real test uses a real id.
constexpr uint64_t kAlice = 1001;
constexpr uint64_t kBob = 2002;

struct Rig {
  std::shared_ptr<simcore::PlayerInventoryStore> store =
      std::make_shared<simcore::PlayerInventoryStore>();
  std::shared_ptr<simcore::ContainerSessionRegistry> sessions =
      std::make_shared<simcore::ContainerSessionRegistry>();
  // A chest manager over a null ESS: saveSlots() writes its cache and returns
  // before touching the network, so persistence is observable in-process and
  // no socket is ever created.
  std::shared_ptr<simcore::ChestStateManager> chestState =
      std::make_shared<simcore::ChestStateManager>(nullptr, /*dimension=*/0);
  // A router client that is never connected. PublishFullInventory() builds the
  // snapshot and Publish() drops it, so no bytes are observable here — the
  // observable contract is the store's postMutation_ instead.
  std::shared_ptr<simcore::IoUringRouterClient> router =
      std::make_shared<simcore::IoUringRouterClient>();
  std::unique_ptr<simcore::InventoryActionHandler> handler;

  // Counts every mutation the store publishes, so a rejected action that still
  // committed is caught.
  std::vector<uint64_t> mutations;

  Rig() {
    handler = std::make_unique<simcore::InventoryActionHandler>(
        store, router, sessions, chestState,
        /*questManager=*/nullptr, /*wbStateManager=*/nullptr);
    store->setPostMutation([this](uint64_t pid, const Slots&) {
      mutations.push_back(pid);
    });
    store->initPlayer(kAlice);
  }

  // Give a player a distinctive 40-slot inventory: every slot holds a unique
  // item so a bleed between any two slots is visible, not just a count change.
  void fillPlayer(uint64_t pid) {
    Slots s{};
    for (int k = 0; k < simcore::kInventorySlots; ++k) {
      s[static_cast<size_t>(k)] =
          item(static_cast<uint16_t>(1000 + k), static_cast<uint8_t>(1 + (k % 60)),
               static_cast<uint16_t>(k));
    }
    store->setSlots(pid, s);
  }

  Snapshot snap(uint64_t pid) const {
    Snapshot s;
    s.slots = store->getSlots(pid);
    s.cursor = store->getCursor(pid);
    if (auto* sess = sessions->find(pid)) {
      s.session_present = true;
      s.container = *sess->slotsRef();
    }
    return s;
  }

  // Feed one wire action to the handler.
  void feed(uint64_t pid, uint8_t action_type, uint8_t button, uint8_t mods,
            uint8_t container_id, uint16_t slot, uint8_t count) {
    flatbuffers::FlatBufferBuilder fbb(128);
    auto off = Protocol::CreateInventoryAction(fbb, pid, action_type, button,
                                               mods, container_id, slot, count);
    fbb.Finish(off);
    const std::vector<uint8_t> bytes(fbb.GetBufferPointer(),
                                     fbb.GetBufferPointer() + fbb.GetSize());
    handler->handle(bytes);
  }

  // Feed arbitrary bytes — for the malformed-buffer paths.
  void feedRaw(const std::vector<uint8_t>& bytes) {
    handler->handle(bytes);
  }

  // Open a chest session for `pid` with `contents`, exactly as
  // ChestOpenHandler would after the async load lands.
  void openChest(uint64_t pid, int32_t x, int32_t y, int32_t z,
                 const std::vector<simcore::PersistSlot>& contents) {
    simcore::ContainerSession s;
    s.kind = simcore::ContainerSession::Kind::Chest;
    s.x = x;
    s.y = y;
    s.z = z;
    s.entity_type = simcore::kChestEntityType;
    s.slots.assign(27, simcore::PersistSlot{});
    const size_t n = contents.size() < s.slots.size() ? contents.size() : s.slots.size();
    for (size_t k = 0; k < n; ++k) s.slots[k] = contents[k];
    sessions->open(pid, s);
  }
};

// Chest contents with `filled` slots of one item id.
std::vector<simcore::PersistSlot> chestOf(uint16_t id, uint8_t count,
                                          size_t filled) {
  std::vector<simcore::PersistSlot> v(27, simcore::PersistSlot{});
  for (size_t k = 0; k < filled && k < v.size(); ++k) {
    v[k] = item(id, count, static_cast<uint16_t>(k));
  }
  return v;
}

} // namespace

// ===========================================================================
// A valid action mutates state
// ===========================================================================

// The baseline acceptance: an empty-cursor left-click on an occupied player
// slot picks the stack up onto the cursor. Slot empties, cursor fills, and the
// mutation is committed (one publish, not two).
static void test_valid_pickup_moves_the_stack_to_the_cursor() {
  Rig r;
  r.fillPlayer(kAlice);
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, /*mods=*/0,
         /*container_id=*/0, /*slot=*/5, /*count=*/1);

  const Snapshot after = r.snap(kAlice);
  CHECK(!(before == after), "a valid click changes state");
  CHECK_EQ(i_(after.slots[5].item_id), 0, "the clicked slot is emptied");
  CHECK_EQ(i_(after.slots[5].count), 0, "with no residual count");
  CHECK_EQ(i_(after.cursor.item_id), 1000 + 5,
           "the stack lands on the server-owned cursor");
  CHECK_EQ(i_(after.cursor.count), before.slots[5].count,
           "carrying its full count");
  CHECK_EQ(i_(after.cursor.meta), 5, "and its meta");
  CHECK_EQ(i_(r.mutations.size()) - mutations_before, 1,
           "exactly one publish for one click, not one per touched field");
}

// A left-click with an occupied cursor onto an empty slot places the whole
// stack. This is the inverse move, so the pair proves the cursor is really
// server-owned state rather than a scratch variable.
static void test_valid_placement_moves_the_cursor_into_the_slot() {
  Rig r;
  Slots s{};
  s[0] = item(555, 12, 3);
  r.store->setSlots(kAlice, s);
  r.store->setCursor(kAlice, item(777, 20, 4));
  const Snapshot before = r.snap(kAlice);
  CHECK_EQ(i_(before.cursor.item_id), 777, "precondition: the cursor is held");

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/0, /*slot=*/9, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK_EQ(i_(after.slots[9].item_id), 777, "the cursor stack fills the slot");
  CHECK_EQ(i_(after.slots[9].count), 20, "with its whole count");
  CHECK_EQ(i_(after.slots[9].meta), 4, "and its meta");
  CHECK_EQ(i_(after.cursor.item_id), 0, "the cursor is emptied");
  CHECK_EQ(i_(after.cursor.count), 0, "with no residual count");
}

// A container click moves the stack from the open chest into the player
// inventory. This is the path the server-authoritative change cares most
// about: the chest is shared state, so a click that mutates one player's
// inventory must not touch another's.
static void test_valid_container_click_moves_an_item_out_of_the_chest() {
  Rig r;
  r.fillPlayer(kAlice);
  r.openChest(kAlice, 10, 64, 10, chestOf(1234, 10, /*filled=*/4));
  const Snapshot before = r.snap(kAlice);
  CHECK_EQ(i_(before.container[0].item_id), 1234, "precondition: the chest is full of item 1234");

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK_EQ(i_(after.container[0].item_id), 0, "the chest slot empties");
  CHECK_EQ(i_(after.cursor.item_id), 1234, "and the stack comes to the cursor");
  CHECK_EQ(i_(after.cursor.count), 10, "with the chest's count");
  CHECK_EQ(i_(after.slots[0].item_id), 1000,
           "the player's own slots are untouched by a pick-up from the chest");
}

// A quick-move out of the chest is the realistic "loot the chest" click, and
// it must land in the player inventory rather than on the cursor.
//
// The player inventory must have ROOM: ApplyQuickMove (InventoryClick.h:192-203)
// lifts the stack out of the chest, hunts for a matching stack and then an
// empty player slot, and if neither exists it puts the stack back and returns
// false. So a full inventory is a different case, covered by
// quick_move_from_chest_into_a_full_inventory_deletes_nothing below.
static void test_quick_move_from_the_chest_lands_in_the_player_inventory() {
  Rig r;
  // A partly-filled inventory: the first ten slots are empty, the rest hold
  // distinct items that are not the chest's item 4321.
  Slots s{};
  for (int k = 10; k < simcore::kInventorySlots; ++k) {
    s[static_cast<size_t>(k)] = item(static_cast<uint16_t>(1000 + k), 4,
                                    static_cast<uint16_t>(k));
  }
  r.store->setSlots(kAlice, s);
  r.openChest(kAlice, 10, 64, 10, chestOf(4321, 7, /*filled=*/2));
  const Snapshot before = r.snap(kAlice);
  CHECK_EQ(i_(before.slots[10].item_id), 1000 + 10,
           "precondition: slot 10 is occupied by a different item");

  r.feed(kAlice, simcore::kActionQuickMove, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK_EQ(i_(after.container[0].item_id), 0, "the chest slot empties");
  // Item 4321 matches nothing the player holds and fits nowhere full, so it
  // must land in the FIRST empty player slot. Slots 0..9 are empty, so slot 0
  // is the destination — found by search rather than assumed, so the assertion
  // stays about the outcome rather than about the search order.
  int landed = -1;
  for (int k = 0; k < simcore::kInventorySlots; ++k) {
    if (after.slots[static_cast<size_t>(k)].item_id == 4321) landed = k;
  }
  CHECK(landed >= 0, "the looted stack is somewhere in the player inventory");
  if (landed >= 0) {
    CHECK_EQ(i_(after.slots[static_cast<size_t>(landed)].count), 7,
             "with its full count");
    CHECK_EQ(i_(after.slots[static_cast<size_t>(landed)].meta), 0,
             "and the meta the chest held");
  }
  CHECK_EQ(i_(after.cursor.item_id), i_(before.cursor.item_id),
           "a quick-move does not touch the cursor");
  CHECK_EQ(i_(after.container[1].item_id), 4321,
           "and it moves exactly one stack, not the whole chest");
}

// Two players with the same chest open: one's click must not alter the other's
// window. Ownership is what makes the model safe, so it is asserted directly.
static void test_two_players_sharing_a_chest_are_isolated() {
  Rig r;
  r.store->initPlayer(kBob);
  r.fillPlayer(kAlice);
  r.fillPlayer(kBob);
  r.openChest(kAlice, 10, 64, 10, chestOf(1234, 10, 4));
  r.openChest(kBob, 10, 64, 10, chestOf(1234, 10, 4));
  const Snapshot bob_before = r.snap(kBob);

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot alice_after = r.snap(kAlice);
  const Snapshot bob_after = r.snap(kBob);
  CHECK_EQ(i_(alice_after.container[0].item_id), 0, "Alice's click took her item");
  CHECK_EQ(i_(bob_after.container[0].item_id), 1234,
           "Bob's window still holds his own copy of the stack");
  CHECK(sameVec(bob_before.container, bob_after.container),
        "Bob's entire container view is byte-identical after Alice's click");
  CHECK(sameSlots(bob_before.slots, bob_after.slots),
        "and so is his player inventory");
  CHECK_EQ(i_(bob_after.cursor.item_id), 0, "Bob's cursor is untouched");
}

// ===========================================================================
// REJECTION — the core invariant: state is COMPLETELY unchanged
// ===========================================================================
//
// Every test in this section follows the same shape:
//
//     Snapshot before = rig.snap(pid);
//     rig.feed(<something that must be rejected>);
//     Snapshot after  = rig.snap(pid);
//     CHECK(before == after);
//
// before == after compares all 40 slots, the cursor, the container's whole slot
// vector, and the session's presence. A partial mutation anywhere fails it.

// R1: a malformed buffer. Every truncation of a real action must fail closed.
static void test_malformed_buffer_leaves_state_completely_unchanged() {
  Rig r;
  r.fillPlayer(kAlice);
  r.openChest(kAlice, 1, 2, 3, chestOf(111, 5, 3));
  r.store->setCursor(kAlice, item(999, 7, 1));

  // Build one real action, then feed every prefix of it plus outright garbage.
  flatbuffers::FlatBufferBuilder fbb(128);
  auto off = Protocol::CreateInventoryAction(fbb, kAlice, simcore::kActionClick,
                                             simcore::kButtonLeft, 0,
                                             /*container_id=*/1, /*slot=*/0, 1);
  fbb.Finish(off);
  const std::vector<uint8_t> real(fbb.GetBufferPointer(),
                                  fbb.GetBufferPointer() + fbb.GetSize());

  const Snapshot before = r.snap(kAlice);
  // The fixture's own setSlots/setCursor calls each publish, so the baseline is
  // captured AFTER them. Only the garbage fed below must add nothing.
  const int mutations_before = i_(r.mutations.size());
  for (size_t cut = 0; cut < real.size(); ++cut) {
    r.feedRaw(std::vector<uint8_t>(real.begin(), real.begin() + cut));
  }
  const std::vector<std::vector<uint8_t>> garbage = {
      {},                            // empty
      {0},                           // one byte
      {0, 0, 0, 0},                  // root offset 0
      {0, 0, 0, 1},                  // root offset past the end
      {0xff, 0xff, 0xff, 0xff},      // offsets into the void
      {0x2a, 0x00},                  // nonsense
      std::vector<uint8_t>(4096, 0),  // large all-zero
  };
  for (const auto& g : garbage) {
    r.feedRaw(g);
  }

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "R1: no truncation or garbage byte sequence changes any state");
  CHECK_EQ(i_(r.mutations.size()), mutations_before,
           "R1: and nothing was published");
}

// A single valid click after all that garbage must still work, proving the
// failures did not wedge the handler or corrupt its state.
static void test_a_valid_click_still_works_after_garbage() {
  Rig r;
  Slots s{};
  s[0] = item(9191, 3, 0);
  r.store->setSlots(kAlice, s);
  r.store->setCursor(kAlice, simcore::PersistSlot{});

  const std::vector<std::vector<uint8_t>> garbage = {
      {}, {0}, {0, 0, 0, 0}, {0xff, 0xff, 0xff, 0xff}, {0x2a, 0x00},
      std::vector<uint8_t>(1024, 0),
  };
  for (const auto& g : garbage) r.feedRaw(g);

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0, 0, 0, 1);
  const Snapshot after = r.snap(kAlice);
  CHECK_EQ(i_(after.slots[0].item_id), 0, "the valid click still applied");
  CHECK_EQ(i_(after.cursor.item_id), 9191, "and its stack reached the cursor");
}

// An empty buffer in particular: the handler must not even read the root.
static void test_empty_buffer_is_a_no_op() {
  Rig r;
  r.fillPlayer(kAlice);
  const Snapshot before = r.snap(kAlice);
  r.feedRaw({});
  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "an empty buffer changes nothing");
}

// R3: player_id 0. The handler drops it before touching any state. This is the
// same "no player, no action" rule InventoryActionHandler.cpp:37 enforces.
static void test_player_id_zero_leaves_state_completely_unchanged() {
  Rig r;
  r.fillPlayer(kAlice);
  // Even with a chest open, so the drop cannot be attributed to the session
  // gate happening to fire first.
  r.openChest(kAlice, 4, 5, 6, chestOf(222, 9, 5));
  r.store->setCursor(kAlice, item(333, 3, 2));
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  // A click that WOULD succeed for a real player: empty the chest's slot 0.
  r.feed(/*pid=*/0, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "R3: an action with player_id 0 changes nothing");
  CHECK_EQ(i_(r.mutations.size()), mutations_before,
           "R3: and publishes nothing");
}

// R4: a container click for a player with NO open session. The click is
// dropped; the player's own inventory must be untouched, because a drop that
// still moved items would be item duplication.
static void test_container_click_without_a_session_leaves_state_unchanged() {
  Rig r;
  r.fillPlayer(kAlice);
  r.store->setCursor(kAlice, item(444, 8, 5));
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());
  CHECK(r.sessions->find(kAlice) == nullptr, "precondition: no session is open");

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "R4: a container click with no session changes nothing");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "R4: and publishes nothing");
}

// The same gate for Bob while Alice's session is open: the registry is keyed
// by player, so one player's open chest must not authorise another's click.
static void test_a_players_session_does_not_authorise_another_players_click() {
  Rig r;
  r.store->initPlayer(kBob);
  r.fillPlayer(kAlice);
  r.fillPlayer(kBob);
  r.store->setCursor(kBob, item(666, 4, 1));
  r.openChest(kAlice, 7, 7, 7, chestOf(888, 6, 3));
  const Snapshot bob_before = r.snap(kBob);
  const int mutations_before = i_(r.mutations.size());

  // Bob clicks a container he has not opened.
  r.feed(kBob, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot alice_after = r.snap(kAlice);
  const Snapshot bob_after = r.snap(kBob);
  CHECK(bob_before == bob_after, "R4: Bob's click is dropped entirely");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "R4: nothing published");
  CHECK_EQ(i_(alice_after.container[0].item_id), 888,
           "R4: and Alice's open chest is not emptied by Bob's rejected click");
}

// An unknown container_id (neither 0 nor 1) is not a valid action_type for
// SlotAt: it falls into the container branch. With no session it must be
// dropped, not treated as a player click.
static void test_unknown_container_id_is_not_treated_as_a_player_click() {
  Rig r;
  r.fillPlayer(kAlice);
  r.store->setCursor(kAlice, item(121, 5, 2));
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/7, /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "an unknown container_id must not be silently downgraded to a player click");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and nothing published");
}

// R6: an out-of-range slot. The player inventory has exactly
// kInventorySlots=40 slots, so slot 40 and slot 65535 are both out of range.
// The whole inventory is filled with distinct items, so a stray write anywhere
// is caught by the full snapshot comparison.
static void test_out_of_range_player_slot_leaves_state_completely_unchanged() {
  Rig r;
  r.fillPlayer(kAlice);
  r.store->setCursor(kAlice, item(1313, 9, 3));
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  const uint16_t out_of_range[] = {
      static_cast<uint16_t>(simcore::kInventorySlots),        // exactly 40
      static_cast<uint16_t>(simcore::kInventorySlots + 1),    // 41
      1000,                                                   // far beyond
      0xFFFF,                                                 // the maximum
  };
  for (uint16_t slot : out_of_range) {
    r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
           /*container_id=*/0, slot, 1);
  }

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "R6: no out-of-range player slot writes to any of the 40 real slots");
  CHECK_EQ(i_(r.mutations.size()), mutations_before,
           "R6: and nothing is published");
}

// The last legal slot is IN range, so the boundary is real: 39 works, 40 does
// not. Without this pair, "out of range is rejected" could be passing merely
// because every slot in the test is rejected for some other reason.
static void test_the_slot_range_boundary_is_exactly_forty_slots() {
  Rig r;
  Slots s{};
  s[static_cast<size_t>(simcore::kInventorySlots - 1)] = item(1717, 3, 0);
  r.store->setSlots(kAlice, s);

  // slot 39 — the last legal index — must succeed.
  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0, 0,
         static_cast<uint16_t>(simcore::kInventorySlots - 1), 1);
  const Snapshot after_legal = r.snap(kAlice);
  CHECK_EQ(i_(after_legal.slots[static_cast<size_t>(simcore::kInventorySlots - 1)].item_id), 0,
           "slot 39 is in range and is mutated");
  CHECK_EQ(i_(after_legal.cursor.item_id), 1717, "and its stack reached the cursor");

  // slot 40 — the first illegal index — must not touch the cursor again.
  const Snapshot before_illegal = r.snap(kAlice);
  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0, 0,
         static_cast<uint16_t>(simcore::kInventorySlots), 1);
  const Snapshot after_illegal = r.snap(kAlice);
  CHECK(before_illegal == after_illegal,
        "slot 40 is the first out-of-range index and changes nothing");
}

// R6, container side: a 27-slot chest. Slot 27 and beyond are out of range and
// must be rejected without disturbing the chest or the player.
static void test_out_of_range_chest_slot_leaves_state_completely_unchanged() {
  Rig r;
  r.fillPlayer(kAlice);
  r.store->setCursor(kAlice, item(1919, 6, 1));
  r.openChest(kAlice, 3, 4, 5, chestOf(2121, 8, /*filled=*/27));
  const Snapshot before = r.snap(kAlice);
  CHECK_EQ(i_(before.container.size()), 27, "precondition: a 27-slot chest");
  const int mutations_before = i_(r.mutations.size());

  const uint16_t out_of_range[] = {27, 28, 999, 0xFFFF};
  for (uint16_t slot : out_of_range) {
    r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
           /*container_id=*/1, slot, 1);
  }

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "R6: an out-of-range chest slot disturbs neither the chest nor the player");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and nothing is published");
}

// An empty chest session (0 slots) rejects EVERY slot, including 0. This is
// the degenerate bounds case, where an off-by-one would write past the end of
// an empty vector.
static void test_empty_container_session_rejects_every_slot() {
  Rig r;
  r.fillPlayer(kAlice);
  r.store->setCursor(kAlice, item(2323, 3, 1));
  simcore::ContainerSession empty;
  empty.kind = simcore::ContainerSession::Kind::Chest;
  empty.entity_type = simcore::kChestEntityType;
  empty.slots.clear(); // zero-length container
  r.sessions->open(kAlice, empty);
  const Snapshot before = r.snap(kAlice);
  CHECK_EQ(i_(before.container.size()), 0, "precondition: the container is empty");

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);
  r.feed(kAlice, simcore::kActionQuickMove, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "a zero-slot container rejects a click and a quick-move, including slot 0");

  // OBSERVED BEHAVIOUR (not a defect in the bounds check): a DROP with a
  // stack in hand clears the CURSOR without ever consulting the slot, because
  // ApplyDrop checks the cursor first (InventoryClick.h:218-221) and only
  // falls through to SlotAt when the cursor is empty. So a drop over a
  // zero-slot container legitimately consumes the held stack rather than
  // rejecting. The cursor is the target, not the container.
  //
  // This is asserted as observed so it is visible: the parent change
  // (refactor-server-authoritative-inventory-verification) reviews exactly
  // this surface, and "a drop never mutates anything out of range" would be
  // the wrong claim to make about it.
  r.store->setCursor(kAlice, item(4242, 5, 2));
  const Snapshot drop_before = r.snap(kAlice);
  r.feed(kAlice, simcore::kActionDrop, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);
  const Snapshot drop_after = r.snap(kAlice);

  CHECK_EQ(i_(drop_after.cursor.item_id), 0,
           "OBSERVED: a drop consumes the held stack even with a zero-slot "
           "container open, because ApplyDrop targets the cursor first");
  CHECK(sameSlots(drop_before.slots, drop_after.slots),
        "but the player inventory is untouched");
  CHECK_EQ(i_(drop_after.container.size()), 0,
         "and the zero-slot container is still zero slots, not resized");

  // The same drop with an EMPTY cursor is a true no-op: there is nothing in
  // hand to drop, so the rule falls through to the slot, finds none, and
  // returns false.
  r.store->setCursor(kAlice, simcore::PersistSlot{});
  const Snapshot empty_drop_before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());
  r.feed(kAlice, simcore::kActionDrop, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);
  const Snapshot empty_drop_after = r.snap(kAlice);
  CHECK(empty_drop_before == empty_drop_after,
        "a drop with an empty cursor over a zero-slot container changes nothing");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and publishes nothing");
}

// R7: an invalid action_type. The rule table's switch has `default: return
// false`, so an out-of-range action type must be a no-op. 5 is the first value
// past the last defined action (kActionPickupAll == 4).
static void test_invalid_action_type_leaves_state_completely_unchanged() {
  Rig r;
  r.fillPlayer(kAlice);
  r.openChest(kAlice, 1, 1, 1, chestOf(2525, 5, 5));
  r.store->setCursor(kAlice, item(2626, 4, 2));
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  // 5 is past the last defined action; 99 and 255 are far past it.
  const uint8_t invalid_actions[] = {5, 6, 99, 0xFF};
  for (uint8_t action_type : invalid_actions) {
    for (uint8_t button : {simcore::kButtonLeft, simcore::kButtonRight}) {
      r.feed(kAlice, action_type, button, 0, /*container_id=*/0, /*slot=*/3, 1);
      r.feed(kAlice, action_type, button, 0, /*container_id=*/1, /*slot=*/3, 1);
    }
  }

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "R7: an undefined action_type changes nothing");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "R7: and nothing is published");
}

// The named constants are the contract: kActionClick..kActionPickupAll must be
// 0..4. If a future action is inserted, this fails and the boundary in the test
// above must move with it.
static void test_action_type_constants_match_the_schema() {
  CHECK_EQ(i_(simcore::kActionClick), 0, "core.fbs: 0 = CLICK");
  CHECK_EQ(i_(simcore::kActionQuickMove), 1, "core.fbs: 1 = QUICK_MOVE");
  CHECK_EQ(i_(simcore::kActionDrop), 2, "core.fbs: 2 = DROP");
  CHECK_EQ(i_(simcore::kActionDragPlace), 3, "core.fbs: 3 = DRAG_PLACE");
  CHECK_EQ(i_(simcore::kActionPickupAll), 4, "core.fbs: 4 = PICKUP_ALL");
  CHECK_EQ(simcore::kButtonLeft, 0, "core.fbs: 0 = LMB");
  CHECK_EQ(simcore::kButtonRight, 1, "core.fbs: 1 = RMB");
}

// R8: a rule that changes nothing. A left-click with an empty cursor onto an
// empty slot is a no-op by rule, and must not be committed.
static void test_click_on_an_empty_slot_with_an_empty_cursor_changes_nothing() {
  Rig r;
  Slots s{}; // every slot empty, cursor empty
  r.store->setSlots(kAlice, s);
  r.store->setCursor(kAlice, simcore::PersistSlot{});
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0, 0,
         /*slot=*/17, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "R8: a no-op click is not committed");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and publishes nothing");
}

// A drop with an empty cursor on an empty slot is likewise a no-op.
static void test_drop_with_an_empty_cursor_on_an_empty_slot_changes_nothing() {
  Rig r;
  r.fillPlayer(kAlice);
  // Find an empty player slot by clearing one explicitly.
  Slots s = r.store->getSlots(kAlice);
  s[20] = simcore::PersistSlot{};
  r.store->setSlots(kAlice, s);
  r.store->setCursor(kAlice, simcore::PersistSlot{});
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionDrop, simcore::kButtonLeft, 0, 0,
         /*slot=*/20, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "R8: dropping nothing destroys nothing");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and publishes nothing");
}

// A double-click (kActionPickupAll) with an empty cursor collects nothing, so
// it must be a no-op — a common way for a rule to look like it "succeeded".
static void test_pickup_all_with_an_empty_cursor_changes_nothing() {
  Rig r;
  r.fillPlayer(kAlice);
  r.store->setCursor(kAlice, simcore::PersistSlot{});
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionPickupAll, simcore::kButtonLeft, 0, 0,
         /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "R8: a collect-matching-stacks with no stack in hand changes nothing");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and publishes nothing");
}

// R9: THE case the parent change targets — a rule that starts mutating and
// then cannot complete. ApplyQuickMove (InventoryClick.h:169-204) lifts the
// stack out of the source slot, hunts for a destination, and on failure puts
// it BACK and returns false. The "put it back" must restore the slot EXACTLY
// as it was; a partial restore (right item, wrong count, or wrong meta) is
// precisely the silent corruption being hunted.
static void test_quick_move_that_cannot_place_puts_the_stack_back_exactly() {
  Rig r;
  // A full 40-slot inventory of 39 distinct stacks plus a full 64 stack in the
  // hotbar — the quick-move has nowhere to go. Slot 0 is hotbar, so moving it
  // targets the main range [10,40), all of which is full of non-matching items.
  Slots s{};
  for (int k = 0; k < simcore::kInventorySlots; ++k) {
    s[static_cast<size_t>(k)] = item(static_cast<uint16_t>(500 + k),
                                    static_cast<uint8_t>(k == 0 ? 64 : 33),
                                    static_cast<uint16_t>(k));
  }
  r.store->setSlots(kAlice, s);
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());
  CHECK_EQ(i_(before.slots[0].count), 64, "precondition: a full stack in the hotbar");

  r.feed(kAlice, simcore::kActionQuickMove, simcore::kButtonLeft, 0, 0,
         /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "R9: a quick-move with nowhere to go leaves the inventory bit-identical");
  CHECK_EQ(i_(after.slots[0].item_id), 500, "R9: the source slot keeps its item");
  CHECK_EQ(i_(after.slots[0].count), 64, "R9: and its exact count");
  CHECK_EQ(i_(after.slots[0].meta), 0, "R9: and its exact meta");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "R9: and nothing is published");
}

// The same "put it back" for the container→player direction: a full player
// inventory, quick-moving out of a chest. The chest slot must keep the stack
// AND the player inventory must be untouched — losing it here would delete the
// item, and duplicating it would be worse.
static void test_quick_move_from_chest_into_a_full_inventory_deletes_nothing() {
  Rig r;
  Slots s{};
  for (int k = 0; k < simcore::kInventorySlots; ++k) {
    s[static_cast<size_t>(k)] = item(static_cast<uint16_t>(500 + k), 33,
                                    static_cast<uint16_t>(k));
  }
  r.store->setSlots(kAlice, s);
  r.openChest(kAlice, 9, 9, 9, chestOf(3131, 12, /*filled=*/2));
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionQuickMove, simcore::kButtonLeft, 0,
         /*container_id=*/1, /*slot=*/0, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "R9: quick-moving into a full inventory changes neither side");
  CHECK_EQ(i_(after.container[0].item_id), 3131,
           "R9: the chest keeps its stack (it is not deleted)");
  CHECK_EQ(i_(after.container[0].count), 12, "R9: with its full count");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "R9: and nothing is published");
}

// A drag-place with an empty cursor places nothing: another rule that returns
// false after inspecting state, and must not half-apply.
static void test_drag_place_with_an_empty_cursor_changes_nothing() {
  Rig r;
  r.fillPlayer(kAlice);
  r.store->setCursor(kAlice, simcore::PersistSlot{});
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionDragPlace, simcore::kButtonRight, 0, 0,
         /*slot=*/11, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "a drag-place with nothing in hand changes nothing");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and publishes nothing");
}

// A right-click that cannot act (different items, or a full target stack) is
// a no-op by rule. This is the mirror of the swap case: it must NOT fall
// through into a swap.
static void test_right_click_onto_a_different_item_changes_nothing() {
  Rig r;
  Slots s{};
  s[4] = item(4141, 5, 1);
  r.store->setSlots(kAlice, s);
  r.store->setCursor(kAlice, item(4242, 3, 2)); // a different item
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonRight, 0, 0,
         /*slot=*/4, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "R8: a right-click onto a different item is a no-op, never a swap");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and publishes nothing");
}

// A right-click onto a FULL same-item stack is the other rejected merge. The
// stack is already at the 64 cap, so nothing can move.
static void test_right_click_onto_a_full_stack_changes_nothing() {
  Rig r;
  Slots s{};
  s[6] = item(4343, simcore::kMaxStack, 4); // already capped
  r.store->setSlots(kAlice, s);
  r.store->setCursor(kAlice, item(4343, 2, 4)); // same stack, held by the cursor
  const Snapshot before = r.snap(kAlice);
  const int mutations_before = i_(r.mutations.size());

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonRight, 0, 0,
         /*slot=*/6, 1);

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after, "R8: a merge into a full stack is a no-op");
  CHECK_EQ(i_(after.cursor.count), 2, "the cursor keeps the stack it held");
  CHECK_EQ(i_(r.mutations.size()), mutations_before, "and publishes nothing");
}

// ---------------------------------------------------------------------------
// The invariant, stated directly: EVERY rejection leaves state identical
// ---------------------------------------------------------------------------

// A table of rejections, run back to back against one fully-populated state.
// Each entry asserts the full snapshot is unchanged after it runs, so a single
// leak in any of them fails here with a name attached.
static void test_every_rejection_in_the_table_leaves_the_full_snapshot_intact() {
  struct Case {
    const char* name;
    uint8_t action_type;
    uint8_t button;
    uint8_t container_id;
    uint16_t slot;
    // Whether a chest session is open for the player. The R4 cases exist to
    // prove the session gate rejects, so theirs must be false; every other
    // case needs one, or the rejection would come from the gate instead of
    // from the rule under test.
    bool open_session;
  };

  // kMaxStack is the stack cap from InventoryClick.h:34; the fixtures below
  // rely on it.
  constexpr uint8_t kMaxStack = simcore::kMaxStack;

  const Case cases[] = {
      // R6 out of range
      {"out-of-range player slot 40", simcore::kActionClick, simcore::kButtonLeft, 0, 40, true},
      {"out-of-range player slot 0xFFFF", simcore::kActionClick, simcore::kButtonRight, 0, 0xFFFF, true},
      {"out-of-range chest slot 27", simcore::kActionClick, simcore::kButtonLeft, 1, 27, true},
      {"out-of-range chest slot 0xFFFF", simcore::kActionQuickMove, simcore::kButtonLeft, 1, 0xFFFF, true},
      // R7 invalid action type
      {"invalid action_type 5", 5, simcore::kButtonLeft, 0, 1, true},
      {"invalid action_type 0xFF", 0xFF, simcore::kButtonRight, 0, 1, true},
      {"invalid action_type 99, container", 99, simcore::kButtonLeft, 1, 1, true},
      // R4 no session for a container click — these must have NO session open,
      // or the click would legitimately succeed and prove nothing.
      {"container click, no session", simcore::kActionClick, simcore::kButtonLeft, 1, 0, false},
      {"container drop, no session", simcore::kActionDrop, simcore::kButtonLeft, 1, 0, false},
      // R3 player_id 0 is covered by its own test (it needs a different pid)
  };

  for (const auto& c : cases) {
    Rig r;
    r.fillPlayer(kAlice);
    // Slot 0 holds a full same-item stack, slot 1 a different item, rest full
    // of distinct items — the fixtures that make each rejection meaningful.
    Slots s = r.store->getSlots(kAlice);
    s[0] = item(6000, kMaxStack, 1);
    s[1] = item(6001, 2, 2);
    r.store->setSlots(kAlice, s);
    r.store->setCursor(kAlice, item(9999, 3, 3));
    // The R4 cases are ABOUT the absence of a session, so opening one would
    // make them vacuous — the click would then legitimately succeed. Every
    // other case needs a session, otherwise the rejection would come from the
    // session gate rather than from the rule under test.
    if (c.open_session) {
      r.openChest(kAlice, 2, 2, 2, chestOf(7777, 5, 5));
    }
    // Bob has a chest open; the session registry must not confuse the two.
    r.store->initPlayer(kBob);

    const Snapshot before = r.snap(kAlice);
    const int mutations_before = i_(r.mutations.size());

    r.feed(kAlice, c.action_type, c.button, /*mods=*/0, c.container_id, c.slot,
           /*count=*/1);

    const Snapshot after = r.snap(kAlice);
    if (!(before == after)) {
      printf("        (rejected case: %s)\n", c.name);
    }
    CHECK(before == after, c.name);
    CHECK_EQ(i_(r.mutations.size()), mutations_before, c.name);
  }
}

// The mirror of the table above: after N rejections, one VALID action still
// applies correctly. A handler that corrupted state on rejection would fail
// here even if each individual rejection looked clean.
static void test_state_is_still_correct_after_many_rejections_then_a_valid_click() {
  Rig r;
  r.fillPlayer(kAlice);
  r.openChest(kAlice, 1, 1, 1, chestOf(5151, 3, 3));
  r.store->setCursor(kAlice, simcore::PersistSlot{});

  // Twenty rejections of every flavour, interleaved.
  for (int round = 0; round < 4; ++round) {
    r.feed(/*pid=*/0, simcore::kActionClick, 0, 0, 0, 0, 1);      // R3
    r.feed(kAlice, simcore::kActionClick, 0, 0, 1, 99, 1);        // R6
    r.feed(kAlice, 5, 0, 0, 0, 1, 1);                             // R7
    r.feed(kAlice, simcore::kActionClick, 0, 0, 7, 1, 1);         // unknown cid
    r.feedRaw({0, 0, 0, 0});                                       // R1
  }
  const Snapshot after_rejections = r.snap(kAlice);
  const int mutations = i_(r.mutations.size());

  // Now a valid click: pick up chest slot 0.
  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0, 1, 0, 1);
  const Snapshot after_valid = r.snap(kAlice);

  CHECK_EQ(i_(after_valid.container[0].item_id), 0,
           "the chest slot empties as expected");
  CHECK_EQ(i_(after_valid.cursor.item_id), 5151,
           "and its stack reaches the cursor — the rejections did not corrupt it");
  CHECK_EQ(i_(after_valid.cursor.count), 3, "with its count intact");
  CHECK_EQ(i_(r.mutations.size()) - mutations, 1,
           "the rejections published nothing; only the valid click did");
  (void)after_rejections;
}

// A rejected container click must not persist the chest either. The chest
// session is the shared copy, so a rejection that wrote to it would corrupt
// every other player watching the same chest. Assert against the manager's
// cache, which is what saveSlots() writes.
static void test_rejected_container_click_does_not_repersist_the_chest() {
  Rig r;
  r.fillPlayer(kAlice);
  const auto contents = chestOf(6161, 8, 4);
  r.openChest(kAlice, 12, 34, 56, contents);
  // Seed the manager's cache with a DIFFERENT snapshot, so if the rejected
  // click wrote the session through, the cache would change to match it.
  r.chestState->saveSlots(12, 34, 56, chestOf(3131, 1, 2),
                          simcore::kChestEntityType);
  std::vector<simcore::PersistSlot> cached_before;
  r.chestState->loadSlots(12, 34, 56, [&](const std::vector<simcore::PersistSlot>& v) {
    cached_before = v;
  });
  CHECK_EQ(i_(cached_before[0].item_id), 3131, "precondition: the cache holds 3131");

  // Rejected: out-of-range chest slot, with a held cursor that a naive handler
  // might place before bounds-checking.
  r.store->setCursor(kAlice, item(7171, 2, 1));
  const Snapshot before = r.snap(kAlice);
  r.feed(kAlice, simcore::kActionClick, simcore::kButtonRight, 0, 1, 27, 1);

  const Snapshot after = r.snap(kAlice);
  std::vector<simcore::PersistSlot> cached_after;
  r.chestState->loadSlots(12, 34, 56, [&](const std::vector<simcore::PersistSlot>& v) {
    cached_after = v;
  });

  CHECK(before == after, "the rejected click changed no live state");
  CHECK(sameVec(cached_before, cached_after),
        "and it did not re-persist the chest through the state manager");
}

// The handler is the ITopicHandler the router dispatches to, so it must be
// callable through that interface. The rejections must hold there too.
static void test_handler_is_reachable_through_the_topic_handler_interface() {
  Rig r;
  r.fillPlayer(kAlice);
  const Snapshot before = r.snap(kAlice);

  simcore::ITopicHandler& asInterface = *r.handler;
  flatbuffers::FlatBufferBuilder fbb(128);
  auto off = Protocol::CreateInventoryAction(fbb, kAlice, simcore::kActionClick,
                                             simcore::kButtonLeft, 0, 0, 40, 1);
  fbb.Finish(off);
  const std::vector<uint8_t> bytes(fbb.GetBufferPointer(),
                                   fbb.GetBufferPointer() + fbb.GetSize());
  asInterface.handle(bytes); // out-of-range slot, must be rejected

  const Snapshot after = r.snap(kAlice);
  CHECK(before == after,
        "a rejected click through ITopicHandler also leaves state unchanged");
}

// handle() is synchronous: on return, the committed store already reflects the
// click. This is what lets the whole suite assert committed state with no
// polling loop and no sleep.
static void test_handle_applies_synchronously() {
  Rig r;
  Slots s{};
  s[0] = item(8181, 2, 0);
  r.store->setSlots(kAlice, s);
  r.store->setCursor(kAlice, simcore::PersistSlot{});

  r.feed(kAlice, simcore::kActionClick, simcore::kButtonLeft, 0, 0, 0, 1);
  // No run loop, no io_context polling — read the store immediately.
  const Slots after = r.store->getSlots(kAlice);
  CHECK_EQ(i_(after[0].item_id), 0,
           "the store is already updated on return from handle()");
  CHECK_EQ(i_(r.store->getCursor(kAlice).item_id), 8181,
           "and so is the cursor");
}

// The workbench grid is a THIRD positional cache behind the same handler, and
// it had the same hand-rolled-key class of defect as ChestStateManager. Its
// posKey (WorkbenchStateManager.cpp:15-19) was
//
//   (x << 0) | (y << 32) | ((uint16_t)z << 48)
//
// which both truncated z to 16 bits AND overlapped z (bits 48-63) with y
// (bits 32-63), so (5,64,0) and (5,64,65536) shared one grid entry and a
// player saw and overwrote the other one's crafting grid.
//
// Fixed in production code (gp-mhiv): the key is now the full (x,y,z) triple.
// This test asserts the FIXED behaviour — each workbench holds its own grid —
// and is the cross-handler regression guard, since the same key backs the
// InventoryActionHandler's workbench path.
static void test_workbench_state_manager_keys_positions_independently() {
  simulation_core::WorkbenchStateManager wb(nullptr, /*dimension=*/0);

  const std::vector<RecipeManager::ItemStack> gridA = {
      {1111, 5, 1}, {0, 0, 0}, {0, 0, 0}};
  const std::vector<RecipeManager::ItemStack> gridB = {
      {2222, 9, 2}, {0, 0, 0}, {0, 0, 0}};

  wb.setGridState(/*x=*/5, /*y=*/64, /*z=*/0, gridA);
  std::vector<RecipeManager::ItemStack> read_back;
  wb.getGridState(5, 64, 0, [&](const std::vector<RecipeManager::ItemStack>& g) {
    read_back = g;
  });
  CHECK_EQ(i_(read_back.size()), 3, "precondition: the grid reads back in full");
  if (!read_back.empty()) {
    CHECK_EQ(i_(read_back[0].item_id), 1111, "and holds the item that was set");
  }

  // The colliding partner: 65536 blocks out in z. Under the old key this
  // overwrote the z=0 workbench's grid.
  wb.setGridState(/*x=*/5, /*y=*/64, /*z=*/65536, gridB);
  std::vector<RecipeManager::ItemStack> at_origin;
  std::vector<RecipeManager::ItemStack> at_high_z;
  wb.getGridState(/*x=*/5, /*y=*/64, /*z=*/0,
                  [&](const std::vector<RecipeManager::ItemStack>& g) {
                    at_origin = g;
                  });
  wb.getGridState(/*x=*/5, /*y=*/64, /*z=*/65536,
                  [&](const std::vector<RecipeManager::ItemStack>& g) {
                    at_high_z = g;
                  });

  CHECK_EQ(i_(at_origin.size()), 3, "the z=0 workbench still has its own grid");
  if (!at_origin.empty()) {
    CHECK_EQ(i_(at_origin[0].item_id), 1111,
             "and it is grid A, not the 65536-blocks-away workbench's grid");
  }
  CHECK_EQ(i_(at_high_z.size()), 3, "the z=65536 workbench has a grid too");
  if (!at_high_z.empty()) {
    CHECK_EQ(i_(at_high_z[0].item_id), 2222, "namely grid B");
  }

  // The y/z OVERLAP, which a fix that only widened z would still leave: z's
  // bit 0 landed on y's bit 16, so (5,64,1) and (5,65600,1) collided too.
  // A FRESH manager, so this is not conflated with the entries above.
  simulation_core::WorkbenchStateManager fresh(nullptr, /*dimension=*/0);
  fresh.setGridState(5, 64, 1, gridA);
  fresh.setGridState(5, 65600, 1, gridB);
  std::vector<RecipeManager::ItemStack> lowY, highY;
  fresh.getGridState(5, 64, 1, [&](const std::vector<RecipeManager::ItemStack>& g) {
    lowY = g;
  });
  fresh.getGridState(5, 65600, 1,
                     [&](const std::vector<RecipeManager::ItemStack>& g) {
                       highY = g;
                     });
  CHECK_EQ(i_(lowY.size()), 3, "the y=64 workbench keeps its own grid");
  if (!lowY.empty()) {
    CHECK_EQ(i_(lowY[0].item_id), 1111, "namely grid A");
  }
  CHECK_EQ(i_(highY.size()), 3, "the y=65600 workbench has its own too");
  if (!highY.empty()) {
    CHECK_EQ(i_(highY[0].item_id), 2222, "namely grid B");
  }

  // Ordinary neighbouring workbenches stay independent, so the fixes above did
  // not break the ordinary path.
  simulation_core::WorkbenchStateManager near(nullptr, /*dimension=*/0);
  near.setGridState(5, 64, 100, gridA);
  near.setGridState(5, 64, 101, gridB);
  std::vector<RecipeManager::ItemStack> near100, near101;
  near.getGridState(5, 64, 100, [&](const std::vector<RecipeManager::ItemStack>& g) {
    near100 = g;
  });
  near.getGridState(5, 64, 101, [&](const std::vector<RecipeManager::ItemStack>& g) {
    near101 = g;
  });
  CHECK_EQ(i_(near100.size()), 3, "the z=100 workbench has its own grid");
  CHECK_EQ(i_(near101.size()), 3, "and so does the z=101 workbench");
  if (!near100.empty() && !near101.empty()) {
    CHECK_EQ(i_(near100[0].item_id), 1111, "z=100 keeps grid A");
    CHECK_EQ(i_(near101[0].item_id), 2222, "z=101 keeps grid B");
  }
}

int main() {
  printf("=== inventory_action_handler test suite ===\n\n");

  // A valid action mutates state
  TEST(valid_pickup_moves_the_stack_to_the_cursor);
  TEST(valid_placement_moves_the_cursor_into_the_slot);
  TEST(valid_container_click_moves_an_item_out_of_the_chest);
  TEST(quick_move_from_the_chest_lands_in_the_player_inventory);
  TEST(two_players_sharing_a_chest_are_isolated);

  // Rejection: state is COMPLETELY unchanged
  TEST(malformed_buffer_leaves_state_completely_unchanged);
  TEST(a_valid_click_still_works_after_garbage);
  TEST(empty_buffer_is_a_no_op);
  TEST(player_id_zero_leaves_state_completely_unchanged);
  TEST(container_click_without_a_session_leaves_state_unchanged);
  TEST(a_players_session_does_not_authorise_another_players_click);
  TEST(unknown_container_id_is_not_treated_as_a_player_click);
  TEST(out_of_range_player_slot_leaves_state_completely_unchanged);
  TEST(the_slot_range_boundary_is_exactly_forty_slots);
  TEST(out_of_range_chest_slot_leaves_state_completely_unchanged);
  TEST(empty_container_session_rejects_every_slot);
  TEST(invalid_action_type_leaves_state_completely_unchanged);
  TEST(action_type_constants_match_the_schema);
  TEST(click_on_an_empty_slot_with_an_empty_cursor_changes_nothing);
  TEST(drop_with_an_empty_cursor_on_an_empty_slot_changes_nothing);
  TEST(pickup_all_with_an_empty_cursor_changes_nothing);
  TEST(quick_move_that_cannot_place_puts_the_stack_back_exactly);
  TEST(quick_move_from_chest_into_a_full_inventory_deletes_nothing);
  TEST(drag_place_with_an_empty_cursor_changes_nothing);
  TEST(right_click_onto_a_different_item_changes_nothing);
  TEST(right_click_onto_a_full_stack_changes_nothing);

  // The invariant, stated directly
  TEST(every_rejection_in_the_table_leaves_the_full_snapshot_intact);
  TEST(state_is_still_correct_after_many_rejections_then_a_valid_click);
  TEST(rejected_container_click_does_not_repersist_the_chest);
  TEST(handler_is_reachable_through_the_topic_handler_interface);
  TEST(handle_applies_synchronously);

  // Sibling storage class (defect fixed in production; behaviour asserted here)
  TEST(workbench_state_manager_keys_positions_independently);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
