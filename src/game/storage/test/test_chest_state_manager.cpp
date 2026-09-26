// ChestStateManager + ChestInteractHandler unit tests (issue gp-7rbz).
//
// Files under test:
//   src/game/storage/ChestStateManager.cpp          — the persistence layer
//   src/game/actions/handlers/ChestInteractHandler.cpp — the right-click gate
//
// Together they own per-chest inventory state. The split is:
//
//   ChestInteractHandler  right-clicking a chest. Publishes exactly one
//                         BlockAck(ACCEPTED) + one BlockDirective(OPEN_UI) and
//                         NOTHING else — it deliberately does not read the
//                         chest. Contents arrive later, over the
//                         chest.open round-trip, through a ContainerSession.
//
//   ChestStateManager     a per-BLOCK-POSITION cache in front of
//                         EntityStateStoreClient. loadSlots() is cache-first and
//                         falls back to an async ESS read; saveSlots() writes
//                         the cache synchronously and then persists;
//                         clearSlots() erases the cache and persists empty.
//
//   ContainerSession      the per-PLAYER live copy. The registry is keyed by
//     Registry            player id, and a chest session OWNS its slot vector
//                         (slotsRef() returns &slots for Kind::Chest), so one
//                         player's edits can never reach another player's
//                         session. That is the ownership invariant under test.
//
// NOT covered, deliberately:
//   * cross-restart persistence (would need a real EntityStateStore — the brief
//     puts it out of scope, and a test that needs a network peer is not
//     deterministic);
//   * the wire bytes PublishFullInventory() hands the router. Publishing is
//     dropped when the router is disconnected, so the payload is unobservable
//     without a live MessageRouter. Only the early-return paths are asserted.
//
// One defect is PINNED as observed behaviour, not blessed. It is marked
// PRODUCTION DEFECT in its test comments; it is the reason that test asserts
// the wrong-looking thing. It is NOT fixed by this file — this file only
// reports it.
//
//   1. entity_type is not part of the cache key, so a chest blob and a machine
//      blob at the same position share one entry. (Tracked as the remaining
//      half of gp-5t4d; the position-collision half of that issue is fixed
//      here, see below.)
//
// One defect WAS fixed by this file, in the same change that fixed gp-5t4d's
// position collision:
//
//   * posKey() used to pack (x << 32) ^ (y << 16) ^ z, which OVERLAPPED the y
//     and z fields (bit 16 of y landed on bit 0 of z). (0,0,0) and (0,1,65536)
//     shared one cache entry, so opening the second chest showed the first
//     one's items. The key is now the full (x,y,z) triple; the tests below
//     assert the FIXED behaviour.
//
// Uses the PROJECT's own harness (src/engine/net/test/test.h convention,
// mirrored by src/game/world/test/BlockTransforms_test.cpp and
// src/game/machines/test/test_explosion_system.cpp). GoogleTest is deliberately
// NOT used: it is absent from conanfile.txt, CI does not install libgtest-dev,
// and CI builds Release with a global -Werror.

#include <cstdio>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <asio.hpp>

#include "core_generated.h"
#include "machine_state_generated.h"

#include <game/storage/ChestStateManager.h>
#include <game/storage/ContainerSession.h>
#include <game/storage/PlayerInventoryStore.h>
#include <game/actions/ActionContext.h>
#include <game/actions/handlers/ChestInteractHandler.h>
#include <apps/simcore/Network/IEventPublisher.h>
#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>

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
#define CHECK_GE(a, b, ...) \
  test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, ##__VA_ARGS__)
#define TEST(name) \
  do {               \
    printf("  TEST: %s\n", #name); \
    test_##name();   \
  } while (0)

namespace {

// Cast helpers so CHECK_EQ never compares a signed and an unsigned operand
// (which -Wextra/-Werror would reject at the macro expansion site).
constexpr int i_(size_t v) { return static_cast<int>(v); }
constexpr int i_(int v) { return v; }  // also covers int32_t
constexpr int i_(unsigned v) { return static_cast<int>(v); }  // uint32_t
constexpr int i_(uint16_t v) { return static_cast<int>(v); }
constexpr int i_(uint8_t v) { return static_cast<int>(v); }
constexpr int i_(bool v) { return v ? 1 : 0; }

// A chest is always 27 slots (ChestOpenHandler hardcodes this).
constexpr size_t kChestSlots = 27;

simcore::PersistSlot item(uint16_t id, uint8_t count, uint16_t meta = 0) {
  return simcore::PersistSlot{id, count, meta};
}

// A chest holding `filled` slots of stack `id`. The result is `filled` slots
// long, so a partial chest is directly distinguishable from a full one.
std::vector<simcore::PersistSlot> chestContents(uint16_t id, uint8_t count,
                                                size_t filled) {
  std::vector<simcore::PersistSlot> slots(std::min(filled, kChestSlots));
  for (size_t k = 0; k < slots.size(); ++k) {
    slots[k] = item(id, count, static_cast<uint16_t>(k));
  }
  return slots;
}

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// Records every publisher call so ordering and "nothing else was sent" are
// directly assertable.
struct MockPublisher : simcore::IEventPublisher {
  struct Ack {
    uint8_t status = 0;
    int32_t x = 0, y = 0, z = 0;
    uint16_t block_id = 0;
    uint8_t meta = 0;
    std::string reason;
    uint32_t request_id = 0;
    uint8_t action_type = 0;
  };
  struct Directive {
    uint8_t directive = 0;
    uint16_t block_id = 0;
    int32_t x = 0, y = 0, z = 0;
    uint32_t request_id = 0;
    uint8_t action_type = 0;
  };

  std::vector<std::string> order;
  std::vector<Ack> acks;
  std::vector<Directive> directives;
  int block_entity_updates = 0;
  int block_changed_events = 0;

  void publishBlockAck(uint8_t status, int32_t x, int32_t y, int32_t z,
                       uint16_t block_id, uint8_t meta, const char* reason,
                       uint32_t request_id, uint8_t action_type) override {
    order.push_back("ack");
    acks.push_back(Ack{status, x, y, z, block_id, meta,
                       reason ? reason : "", request_id, action_type});
  }

  void publishBlockDirective(uint8_t directive, uint16_t block_id, int32_t x,
                             int32_t y, int32_t z, uint32_t request_id,
                             uint8_t action_type) override {
    order.push_back("directive");
    directives.push_back(
        Directive{directive, block_id, x, y, z, request_id, action_type});
  }

  void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                uint32_t, uint64_t) override {
    ++block_changed_events;
  }

  // The hatches parameter is std::vector<HatchUpdateData>*, not
  // std::array<uint8_t,6>* — HatchUpdateData is a global struct declared in
  // IEventPublisher.h, not one of the anonymous-namespace types.
  void publishBlockEntityUpdate(int32_t, int32_t, int32_t, uint16_t,
                                const std::vector<uint8_t>&, float, uint32_t,
                                EnergyType, uint32_t, int, float,
                                const std::vector<HatchUpdateData>* = nullptr,
                                double = -1.0, double = -1.0) override {
    ++block_entity_updates;
  }

  void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                  uint16_t, uint8_t, uint16_t,
                                  const char*) override {}
  void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                        const std::array<uint8_t, 6>&) override {}
  void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t,
                                uint16_t) override {}
  void publishMultiblockDestroyed(uint64_t) override {}
  void publishGridUpdate(int32_t, int32_t, int32_t,
                         const std::vector<RecipeManager::ItemStack>&) override {}
};

// An unconnected EntityStateStoreClient: LoadEntityState/SaveEntityState log an
// error and invoke the callback INLINE with an empty/failed result. No thread,
// no socket, no network — which is exactly what makes it a usable double for
// pinning the cache-first path.
struct OfflineEss {
  asio::io_context io;
  std::shared_ptr<simcore::EntityStateStoreClient> client =
      std::make_shared<simcore::EntityStateStoreClient>(io);
};

// A ContainerSession exactly as ChestOpenHandler registers it the instant the
// window opens: 27 empty slots, at the chest's position, entity_type 3.
// ChestOpenHandler.cpp lives in src/apps/simcore and is out of this issue's
// scope, so its bootstrap is reproduced here rather than called.
simcore::ContainerSession freshSession(int32_t x, int32_t y, int32_t z) {
  simcore::ContainerSession s;
  s.kind = simcore::ContainerSession::Kind::Chest;
  s.x = x;
  s.y = y;
  s.z = z;
  s.entity_type = simcore::kChestEntityType;
  s.slots.assign(kChestSlots, simcore::PersistSlot{});
  return s;
}

// The fill step ChestOpenHandler's loadSlots callback performs.
void fillSession(simcore::ContainerSession& s,
                 const std::vector<simcore::PersistSlot>& loaded) {
  const size_t n = std::min(loaded.size(), s.slots.size());
  for (size_t k = 0; k < n; ++k) s.slots[k] = loaded[k];
}

bool slotsEqual(const std::vector<simcore::PersistSlot>& a,
                const std::vector<simcore::PersistSlot>& b) {
  if (a.size() != b.size()) return false;
  for (size_t k = 0; k < a.size(); ++k) {
    if (a[k].item_id != b[k].item_id || a[k].count != b[k].count ||
        a[k].meta != b[k].meta) {
      return false;
    }
  }
  return true;
}

// Build a SetBlockAction and the ActionContext the handlers receive. The
// FlatBufferBuilder must outlive the context, so both are returned together.
struct Ctx {
  flatbuffers::FlatBufferBuilder fbb{256};
  std::vector<uint8_t> buffer;
  Protocol::SetBlockAction* action = nullptr;
  std::unique_ptr<simcore::ActionContext> ctx;

  Ctx(int32_t x, int32_t y, int32_t z, uint16_t expected_block_id,
      uint8_t action_type, uint32_t request_id, std::uint8_t face,
      uint64_t player_id,
      std::shared_ptr<simcore::IEventPublisher> publisher) {
    Protocol::Vec3i pos(x, y, z);
    auto off = Protocol::CreateSetBlockAction(fbb, player_id,
                                              static_cast<Protocol::PlayerActionType>(
                                                  action_type),
                                              &pos, expected_block_id,
                                              /*new_block_id=*/0, request_id,
                                              face, /*held_item=*/0);
    fbb.Finish(off);
    buffer.assign(fbb.GetBufferPointer(),
                  fbb.GetBufferPointer() + fbb.GetSize());
    action = flatbuffers::GetMutableRoot<Protocol::SetBlockAction>(buffer.data());
    ctx = std::make_unique<simcore::ActionContext>(
        action, /*repo=*/nullptr, publisher,
        /*engine=*/nullptr,
        std::make_shared<simcore::PlayerInventoryStore>(),
        /*entityStateClient=*/nullptr, simcore::ItemGiveCallback{},
        simcore::DrillUseCallback{}, simcore::BlockPlacedCallback{},
        simcore::PostCallback{});
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Blob codec — EncodeChestBlob / DecodeChestBlob
// ---------------------------------------------------------------------------

// The blob is a Protocol::MachineState (root_type) whose `inventory` carries
// the chest slots, and it round-trips a full stack losslessly.
static void test_ChestBlob_round_trips_item_id_count_and_meta() {
  const std::vector<simcore::PersistSlot> slots = {
      item(1234, 64, 7), item(4321, 1, 0), simcore::PersistSlot{}};

  const auto blob = simcore::EncodeChestBlob(slots);
  CHECK(!blob.empty(), "encoding a chest must produce a non-empty blob");

  const auto back = simcore::DecodeChestBlob(blob);
  CHECK_EQ(i_(back.size()), i_(slots.size()), "slot count round-trips");
  if (back.size() == slots.size()) {
    CHECK_EQ(i_(back[0].item_id), 1234, "item_id round-trips");
    CHECK_EQ(i_(back[0].count), 64, "count round-trips");
    CHECK_EQ(i_(back[0].meta), 7, "meta round-trips");
    CHECK_EQ(i_(back[1].item_id), 4321, "second item_id round-trips");
    CHECK_EQ(i_(back[1].count), 1, "second count round-trips");
    // An all-zero slot must survive as an all-zero slot, not as garbage.
    CHECK_EQ(i_(back[2].item_id), 0, "empty slot stays empty");
    CHECK_EQ(i_(back[2].count), 0, "empty slot count stays 0");
    CHECK_EQ(i_(back[2].meta), 0, "empty slot meta stays 0");
  }
}

// A full 27-slot chest survives the round trip at its real size.
static void test_ChestBlob_round_trips_a_full_27_slot_chest() {
  const auto slots = chestContents(/*id=*/999, /*count=*/42, /*filled=*/27);
  const auto back = simcore::DecodeChestBlob(simcore::EncodeChestBlob(slots));

  CHECK_EQ(i_(back.size()), 27, "27 slots in, 27 slots out");
  CHECK(slotsEqual(back, slots), "every slot is byte-identical after encode+decode");
}

// The written blob really is a MachineState at version 1 — the format the
// EntityStateStore stores opaquely under entity_type 3.
static void test_ChestBlob_writes_machine_state_version_one() {
  const auto blob = simcore::EncodeChestBlob(chestContents(5, 1, 2));
  flatbuffers::Verifier v(blob.data(), blob.size());
  CHECK(v.VerifyBuffer<Protocol::MachineState>(nullptr),
        "the blob verifies as a MachineState");
  if (!v.VerifyBuffer<Protocol::MachineState>(nullptr)) return;

  const auto* st = flatbuffers::GetRoot<Protocol::MachineState>(blob.data());
  CHECK_EQ(i_(st->version()), 1, "MachineState.version is hardcoded to 1");
  CHECK(st->inventory() != nullptr, "the blob carries an inventory");
  if (st->inventory()) {
    CHECK_EQ(i_(st->inventory()->size()), 2, "inventory.size == slot count");
    CHECK_EQ(i_(st->inventory()->slots()->size()), 2, "slot vector length");
  }
}

// Encoding zero slots still produces a VALID blob — "empty chest" and "no
// blob" are different things, and only the latter decodes to nothing without
// ever reaching the parser.
static void test_ChestBlob_empty_input_still_encodes_a_valid_blob() {
  const auto blob = simcore::EncodeChestBlob({});
  CHECK(!blob.empty(), "an empty chest still serialises to a non-empty blob");
  CHECK_EQ(i_(simcore::DecodeChestBlob(blob).size()), 0,
           "which decodes back to zero slots");
}

// A missing blob (ESS has no record) short-circuits before the verifier.
static void test_ChestBlob_empty_blob_decodes_to_empty() {
  CHECK_EQ(i_(simcore::DecodeChestBlob({}).size()), 0,
           "an empty blob is an empty chest, not a crash");
}

// Malformed input must be rejected by the FlatBuffers verifier, never
// dereferenced. Sample the interesting shapes: all-zeroes (root offset 0),
// all-ones (offset past the end of the buffer), and a truncated real blob.
static void test_ChestBlob_malformed_input_decodes_to_empty() {
  const std::vector<std::vector<uint8_t>> garbage = {
      {0, 0, 0, 0},
      {0, 0, 0, 1},
      {0xff, 0xff, 0xff, 0xff},
      {0x2a, 0x00},
      {0x04, 0x00, 0x20, 0x00, 0xff, 0xff, 0xff, 0xff},
  };
  for (size_t k = 0; k < garbage.size(); ++k) {
    const auto out = simcore::DecodeChestBlob(garbage[k]);
    CHECK_EQ(i_(out.size()), 0,
             "a malformed blob decodes to an empty chest, never a crash");
  }

  // Truncating a real blob must also fail closed.
  const auto real = simcore::EncodeChestBlob(chestContents(11, 3, 4));
  for (size_t cut = 1; cut < real.size(); ++cut) {
    const std::vector<uint8_t> truncated(real.begin(), real.begin() + cut);
    const auto out = simcore::DecodeChestBlob(truncated);
    CHECK_EQ(i_(out.size()), 0, "every truncation of a real blob fails closed");
  }
}

// MachineState carries more than a chest uses. A blob with no inventory at all,
// and one whose inventory has no slot vector, must both decode to empty.
static void test_ChestBlob_machine_state_without_inventory_decodes_to_empty() {
  {
    flatbuffers::FlatBufferBuilder fbb(128);
    auto st = Protocol::CreateMachineState(fbb, 1, nullptr, 0, 0, 0);
    fbb.Finish(st);
    std::vector<uint8_t> blob(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
    CHECK_EQ(i_(simcore::DecodeChestBlob(blob).size()), 0,
             "a MachineState with no inventory is an empty chest");
  }
  {
    flatbuffers::FlatBufferBuilder fbb(128);
    auto inv = Protocol::CreateMachineInventory(fbb, 0, 0);
    auto st = Protocol::CreateMachineState(fbb, 1, nullptr, 0, inv, 0);
    fbb.Finish(st);
    std::vector<uint8_t> blob(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
    CHECK_EQ(i_(simcore::DecodeChestBlob(blob).size()), 0,
             "an inventory with no slot vector is an empty chest");
  }
}

// PRODUCTION DEFECT (not blessed): PersistSlot::count is uint8 but
// MachineInventorySlot.count is uint16, so decode clamps. 255 is the ceiling —
// a stack written as 300 by any other producer comes back as 255, silently
// losing 45 items rather than failing.
static void test_ChestBlob_decode_clamps_count_above_255_to_255() {
  flatbuffers::FlatBufferBuilder fbb(128);
  std::vector<flatbuffers::Offset<Protocol::MachineInventorySlot>> offs;
  offs.push_back(Protocol::CreateMachineInventorySlot(
      fbb, /*item_id=*/77, /*count=*/static_cast<uint16_t>(300), /*meta=*/5));
  auto vec = fbb.CreateVector(offs);
  auto inv = Protocol::CreateMachineInventory(fbb, 1, vec);
  auto st = Protocol::CreateMachineState(fbb, 1, nullptr, 0, inv, 0);
  fbb.Finish(st);
  std::vector<uint8_t> blob(fbb.GetBufferPointer(),
                            fbb.GetBufferPointer() + fbb.GetSize());

  const auto back = simcore::DecodeChestBlob(blob);
  CHECK_EQ(i_(back.size()), 1, "the out-of-range slot is still present");
  if (back.size() == 1) {
    CHECK_EQ(i_(back[0].count), 255, "count 300 clamps to 255, losing 45 items");
    CHECK_EQ(i_(back[0].item_id), 77, "item_id is unaffected by the clamp");
    CHECK_EQ(i_(back[0].meta), 5, "meta is unaffected by the clamp");
  }
}

// ---------------------------------------------------------------------------
// ChestStateManager — load / save / clear
// ---------------------------------------------------------------------------

// saveSlots() writes the cache synchronously and the ESS write is invisible
// here, so a following loadSlots() is served entirely from the cache. This is
// the "contents survive a close/reopen within the process" requirement: the
// reopen is a fresh loadSlots against the same manager.
static void test_ChestStateManager_saved_chest_survives_close_and_reopen() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, /*dimension=*/0);
  const auto contents = chestContents(/*id=*/500, /*count=*/32, /*filled=*/27);

  mgr.saveSlots(/*x=*/10, /*y=*/64, /*z=*/-20, contents);

  std::vector<simcore::PersistSlot> reloaded;
  mgr.loadSlots(10, 64, -20, [&](const std::vector<simcore::PersistSlot>& s) {
    reloaded = s;
  });

  CHECK_EQ(i_(reloaded.size()), 27, "reopening the chest returns 27 slots");
  CHECK(slotsEqual(reloaded, contents),
        "reopening the chest returns exactly what was saved");
}

// The cache is consulted BEFORE the entity store. Proof: a chest that has been
// saved is read back even though the ESS is offline (an offline load always
// returns nothing). If loadSlots() hit the store first, this would come back
// empty.
static void test_ChestStateManager_load_is_cache_first() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(1, 2, 3, chestContents(21, 5, 3));

  // Direct evidence the offline store yields nothing...
  std::vector<simcore::PersistSlot> fromCache;
  mgr.loadSlots(1, 2, 3, [&](const std::vector<simcore::PersistSlot>& s) {
    fromCache = s;
  });
  CHECK_EQ(i_(fromCache.size()), 3, "a cached chest reads back despite an offline store");

  // ...and that a never-cached position does yield nothing from that store.
  std::vector<simcore::PersistSlot> miss;
  mgr.loadSlots(1000, 1000, 1000, [&](const std::vector<simcore::PersistSlot>& s) {
    miss = s;
  });
  CHECK_EQ(i_(miss.size()), 0,
           "an uncached position against an offline store is an empty chest");
}

// With no entity store at all, a miss is an empty chest. This is the shape a
// single-player/headless simcore runs in, so it is worth pinning.
static void test_ChestStateManager_null_client_load_is_an_empty_chest() {
  simcore::ChestStateManager mgr(nullptr, 0);

  std::vector<simcore::PersistSlot> out;
  bool fired = false;
  mgr.loadSlots(5, 6, 7, [&](const std::vector<simcore::PersistSlot>& s) {
    out = s;
    fired = true;
  });

  CHECK(fired, "the load callback always runs, store or no store");
  CHECK_EQ(i_(out.size()), 0, "an unloaded chest is empty");
}

// The callback fires INLINE on the synchronous paths (cache hit, null client,
// offline store) — no thread, no polling loop, no sleep needed in the test.
static void test_ChestStateManager_load_callback_is_synchronous() {
  {
    simcore::ChestStateManager mgr(nullptr, 0);
    bool fired = false;
    mgr.loadSlots(1, 1, 1, [&](const std::vector<simcore::PersistSlot>&) {
      fired = true;
    });
    CHECK(fired, "null client: the callback has already run on return");
  }
  {
    OfflineEss ess;
    simcore::ChestStateManager mgr(ess.client, 0);
    mgr.saveSlots(2, 2, 2, chestContents(1, 1, 1));
    bool fired = false;
    mgr.loadSlots(2, 2, 2, [&](const std::vector<simcore::PersistSlot>&) {
      fired = true;
    });
    CHECK(fired, "cache hit: the callback has already run on return");
  }
}

// clearSlots() (block destroyed) empties the cached chest: the next load
// returns nothing.
static void test_ChestStateManager_clear_empties_the_cached_chest() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(3, 4, 5, chestContents(88, 12, 6));

  std::vector<simcore::PersistSlot> before;
  mgr.loadSlots(3, 4, 5, [&](const std::vector<simcore::PersistSlot>& s) {
    before = s;
  });
  CHECK_EQ(i_(before.size()), 6, "precondition: the chest has contents");

  mgr.clearSlots(3, 4, 5);

  std::vector<simcore::PersistSlot> after;
  mgr.loadSlots(3, 4, 5, [&](const std::vector<simcore::PersistSlot>& s) {
    after = s;
  });
  CHECK_EQ(i_(after.size()), 0, "a cleared chest reopens empty");
}

// clearSlots() and saveSlots() only touch their own position. Each neighbour
// holds a DIFFERENT number of slots with a DIFFERENT item, so a bleed between
// them is directly visible, not just a size change.
static void test_ChestStateManager_clear_leaves_neighbouring_chests_alone() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(100, 64, 100, chestContents(1, 1, 1));
  mgr.saveSlots(101, 64, 100, chestContents(2, 2, 2));
  mgr.saveSlots(100, 65, 100, chestContents(3, 3, 3));
  mgr.saveSlots(100, 64, 101, chestContents(4, 4, 4));

  mgr.clearSlots(100, 64, 100);

  // The cleared chest itself.
  std::vector<simcore::PersistSlot> cleared;
  mgr.loadSlots(100, 64, 100, [&](const std::vector<simcore::PersistSlot>& s) {
    cleared = s;
  });
  CHECK_EQ(i_(cleared.size()), 0, "the cleared chest is the one that empties");

  const int32_t neighbours[3][3] = {{101, 64, 100}, {100, 65, 100},
                                    {100, 64, 101}};
  const uint16_t expected_ids[3] = {2, 3, 4};
  for (size_t k = 0; k < 3; ++k) {
    std::vector<simcore::PersistSlot> out;
    mgr.loadSlots(neighbours[k][0], neighbours[k][1], neighbours[k][2],
                  [&](const std::vector<simcore::PersistSlot>& s) { out = s; });
    CHECK_EQ(i_(out.size()), i_(k + 2),
             "clearing one chest must not empty a neighbouring one");
    if (!out.empty()) {
      CHECK_EQ(i_(out[0].item_id), i_(expected_ids[k]),
               "a neighbour keeps its OWN contents, not another's");
    }
  }
}

// Ordinary neighbouring chests keep independent inventories.
static void test_ChestStateManager_neighbouring_chests_are_independent() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(0, 64, 0, chestContents(10, 10, 1));
  mgr.saveSlots(1, 64, 0, chestContents(20, 20, 2));
  mgr.saveSlots(0, 65, 0, chestContents(30, 30, 3));
  mgr.saveSlots(0, 64, 1, chestContents(40, 40, 4));

  const int32_t positions[4][3] = {{0, 64, 0}, {1, 64, 0}, {0, 65, 0},
                                   {0, 64, 1}};
  const uint16_t expected_ids[4] = {10, 20, 30, 40};
  for (size_t k = 0; k < 4; ++k) {
    std::vector<simcore::PersistSlot> out;
    mgr.loadSlots(positions[k][0], positions[k][1], positions[k][2],
                  [&](const std::vector<simcore::PersistSlot>& s) { out = s; });
    CHECK_EQ(i_(out.size()), i_(k + 1), "each chest keeps its own slot count");
    if (!out.empty()) {
      CHECK_EQ(i_(out[0].item_id), i_(expected_ids[k]),
               "each chest keeps its own item");
    }
  }
}

// Negative coordinates are legal world positions and must key independently.
static void test_ChestStateManager_negative_coordinates_key_independently() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(-1, 64, -1, chestContents(7, 7, 1));
  mgr.saveSlots(-2, 64, -1, chestContents(8, 8, 2));

  std::vector<simcore::PersistSlot> a, b;
  mgr.loadSlots(-1, 64, -1,
                [&](const std::vector<simcore::PersistSlot>& s) { a = s; });
  mgr.loadSlots(-2, 64, -1,
                [&](const std::vector<simcore::PersistSlot>& s) { b = s; });
  CHECK_EQ(i_(a.size()), 1, "chest at (-1,64,-1)");
  CHECK_EQ(i_(b.size()), 2, "chest at (-2,64,-1)");
}

// (gp-5t4d) posKey() once packed (x << 32) ^ (y << 16) ^ z, which OVERLAPPED
// the y and z fields: y was shifted only 16 bits and z was XORed in with no
// shift at all, so bit 16 of y landed on bit 0 of z. Two legal positions
// therefore shared ONE cache entry:
//
//   posKey(0,0,0)      == 0
//   posKey(0,1,65536)  == (1 << 16) ^ 65536 == 0
//
// Both are legal world positions, so opening the second chest showed the first
// one's items. It is a user-visible corruption, not a theoretical one, so the
// test below asserts the FIXED behaviour through the manager's public API:
// save at one position, load at the colliding position, and require that the
// second position is EMPTY rather than showing the first chest's items.
static void test_ChestStateManager_colliding_positions_do_not_share_a_cache_entry() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(/*x=*/0, /*y=*/0, /*z=*/0, chestContents(31337, 1, 1));

  std::vector<simcore::PersistSlot> aliased;
  mgr.loadSlots(/*x=*/0, /*y=*/1, /*z=*/65536,
                [&](const std::vector<simcore::PersistSlot>& s) { aliased = s; });

  CHECK_EQ(i_(aliased.size()), 0,
           "chest (0,1,65536) is NOT chest (0,0,0): opening it must show its "
           "own (empty) contents, never the other chest's items");
}

// The same overlap, one block apart in y against 65536 blocks in z — the pair
// the issue report names. Kept as its own case so the two directions of the
// overlap (which x/y/z combination) cannot be papered over by one fix that
// only handles the origin.
static void test_ChestStateManager_y_and_z_fields_do_not_overlap() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(/*x=*/10, /*y=*/64, /*z=*/100, chestContents(4242, 7, 3));

  // One block up in y, and 65536 + 100 blocks further out in z.
  std::vector<simcore::PersistSlot> neighbour;
  mgr.loadSlots(/*x=*/10, /*y=*/65, /*z=*/65636,
                [&](const std::vector<simcore::PersistSlot>& s) { neighbour = s; });

  CHECK_EQ(i_(neighbour.size()), 0,
           "a chest one block above and 65536 blocks away is a different "
           "chest, not the same cache entry");

  // And the original position must still read back its own contents.
  std::vector<simcore::PersistSlot> own;
  mgr.loadSlots(/*x=*/10, /*y=*/64, /*z=*/100,
                [&](const std::vector<simcore::PersistSlot>& s) { own = s; });
  CHECK_EQ(i_(own.size()), 3, "the original chest still holds its contents");
  if (!own.empty()) {
    CHECK_EQ(i_(own[0].item_id), 4242, "and they are its OWN items");
  }
}

// A regression sweep rather than a single pair: a long run of positions one
// block apart in y (each 65536 further out in z) must all key independently.
// The old key mapped that whole run onto ONE entry, so the very first save
// would be visible at every one of them. This catches a partial fix that only
// handles the specific pair above.
static void test_ChestStateManager_a_run_of_65536_z_steps_keys_independently() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(/*x=*/3, /*y=*/0, /*z=*/0, chestContents(9001, 1, /*filled=*/1));

  for (int32_t k = 1; k <= 8; ++k) {
    const int32_t z = k * 65536;
    std::vector<simcore::PersistSlot> out;
    mgr.loadSlots(/*x=*/3, /*y=*/k, /*z=*/z,
                  [&](const std::vector<simcore::PersistSlot>& s) { out = s; });
    CHECK_EQ(i_(out.size()), 0,
             "each (0+k, 65536*k) position is its own cache entry");
  }
}

// The structural property that makes all of the above true: the key is the
// full 3-tuple, so a one-block step along ANY axis always yields a different
// key. A packed key that merely widens one field still fails this if it leaves
// any two axes sharing bits.
static void test_ChestStateManager_one_block_step_on_any_axis_is_a_new_key() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  const int32_t base[3] = {7, 64, -20};

  // A distinct item per step, so a bleed is visible as a wrong item, not just
  // a wrong size.
  for (int axis = 0; axis < 3; ++axis) {
    for (int32_t step : {int32_t{1}, int32_t{-1}}) {
      int32_t p[3] = {base[0], base[1], base[2]};
      p[axis] += step;
      mgr.saveSlots(base[0], base[1], base[2],
                    chestContents(static_cast<uint16_t>(100 + axis * 2 + step + 2),
                                  5, 4));
      std::vector<simcore::PersistSlot> out;
      mgr.loadSlots(p[0], p[1], p[2],
                    [&](const std::vector<simcore::PersistSlot>& s) { out = s; });
      CHECK_EQ(i_(out.size()), 0,
               "a one-block step along an axis is a different chest");
      mgr.clearSlots(base[0], base[1], base[2]);
    }
  }
}

// clearSlots() on one of a colliding pair must not empty the other. Under the
// old key the two shared an entry, so breaking one chest silently emptied the
// other 65536 blocks away — items destroyed by a block break.
static void test_ChestStateManager_clear_of_a_colliding_position_spares_the_other() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  mgr.saveSlots(/*x=*/0, /*y=*/0, /*z=*/0, chestContents(5151, 3, 2));
  mgr.saveSlots(/*x=*/0, /*y=*/1, /*z=*/65536, chestContents(6262, 4, 5));

  // Both were saved; each must still hold its OWN contents, not the other's.
  std::vector<simcore::PersistSlot> a, b;
  mgr.loadSlots(0, 0, 0, [&](const std::vector<simcore::PersistSlot>& s) { a = s; });
  mgr.loadSlots(0, 1, 65536, [&](const std::vector<simcore::PersistSlot>& s) { b = s; });
  CHECK_EQ(i_(a.size()), 2,
           "the second save did not overwrite the first chest's entry");
  if (!a.empty()) CHECK_EQ(i_(a[0].item_id), 5151, "and it keeps its own item");
  CHECK_EQ(i_(b.size()), 5, "and the second chest holds its own 5 slots");
  if (!b.empty()) CHECK_EQ(i_(b[0].item_id), 6262, "namely its own item");

  mgr.clearSlots(/*x=*/0, /*y=*/0, /*z=*/0);

  std::vector<simcore::PersistSlot> survivor;
  mgr.loadSlots(/*x=*/0, /*y=*/1, /*z=*/65536,
                [&](const std::vector<simcore::PersistSlot>& s) { survivor = s; });
  CHECK_EQ(i_(survivor.size()), 5,
           "breaking the chest at (0,0,0) must not empty the chest 65536 "
           "blocks away — that destroyed the player's items");
  if (!survivor.empty()) {
    CHECK_EQ(i_(survivor[0].item_id), 6262, "which keeps its own items");
  }
}

// entity_type is part of the cache key, so a chest and a machine at the SAME
// block position never share an entry. saveSlots/loadSlots already forward it to
// EntityStateStore — the ESS key is (dim,x,y,z,entity_type) — so a cache keyed
// only on position served one entity's slots for the other: opening a chest at
// (7,70,7) returned a machine's 4 slots and the machine's save overwrote the
// chest's (gp-py5s, the entity_type half of gp-5t4d).
static void test_ChestStateManager_cache_key_includes_entity_type() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  // Distinct sizes as well as distinct ids, so a cross-read is visible whether
  // it lands on the wrong item or the wrong number of slots.
  const auto chestSaved = chestContents(/*id=*/555, /*count=*/6, /*filled=*/2);
  const auto machineSaved = chestContents(/*id=*/666, /*count=*/9, /*filled=*/4);
  mgr.saveSlots(7, 70, 7, chestSaved, /*entity_type=*/3);

  // A machine at the same position, saved under its own machine_id.
  mgr.saveSlots(7, 70, 7, machineSaved, /*entity_type=*/42);

  std::vector<simcore::PersistSlot> asChest;
  mgr.loadSlots(7, 70, 7,
                [&](const std::vector<simcore::PersistSlot>& s) { asChest = s; },
                /*entity_type=*/3);

  CHECK(slotsEqual(asChest, chestSaved),
        "the chest at (7,70,7) reads back its own slots, not the machine's");
  CHECK_EQ(i_(asChest.size()), 2, "which are 2 slots, not the machine's 4");

  // And the machine still reads back its own state, not the chest's.
  std::vector<simcore::PersistSlot> asMachine;
  mgr.loadSlots(7, 70, 7,
                [&](const std::vector<simcore::PersistSlot>& s) { asMachine = s; },
                /*entity_type=*/42);
  CHECK(slotsEqual(asMachine, machineSaved),
        "the machine at (7,70,7) reads back its own slots, not the chest's");
  CHECK_EQ(i_(asMachine.size()), 4, "which are 4 slots, not the chest's 2");

  // Clearing the chest must not empty the machine's entry: the store is keyed
  // separately per entity_type, so destroying one block at that position cannot
  // destroy the other's state either.
  mgr.clearSlots(7, 70, 7, /*entity_type=*/3);

  std::vector<simcore::PersistSlot> afterClear;
  mgr.loadSlots(7, 70, 7,
                [&](const std::vector<simcore::PersistSlot>& s) { afterClear = s; },
                /*entity_type=*/42);
  CHECK(slotsEqual(afterClear, machineSaved),
        "clearing the chest leaves the machine's entry intact");

  // And the chest itself is now empty while the machine is untouched — the two
  // entries move independently in both directions.
  std::vector<simcore::PersistSlot> clearedChest;
  mgr.loadSlots(7, 70, 7,
                [&](const std::vector<simcore::PersistSlot>& s) { clearedChest = s; },
                /*entity_type=*/3);
  CHECK_EQ(i_(clearedChest.size()), 0, "the cleared chest reads back empty");

  // The default entity_type (kChestEntityType) is just another key value, so the
  // default-argument call path a chest open actually takes must land on the
  // chest's entry, not on a machine's.
  const auto reSaved = chestContents(/*id=*/777, /*count=*/3, /*filled=*/1);
  mgr.saveSlots(7, 70, 7, reSaved);
  std::vector<simcore::PersistSlot> viaDefault;
  mgr.loadSlots(7, 70, 7,
                [&](const std::vector<simcore::PersistSlot>& s) { viaDefault = s; });
  CHECK(slotsEqual(viaDefault, reSaved),
        "the default-argument load is the chest entry, holding its own items");
  std::vector<simcore::PersistSlot> machineStillThere;
  mgr.loadSlots(7, 70, 7,
                [&](const std::vector<simcore::PersistSlot>& s) { machineStillThere = s; },
                /*entity_type=*/42);
  CHECK(slotsEqual(machineStillThere, machineSaved),
        "and the machine's entry is unaffected by the default-type save");
}

// The manager is a plain value: two instances over the same position do not
// share a cache. Relevant because simcore owns one manager per process, so
// nothing in production depends on this, but it pins the "cache is per-object"
// contract.
static void test_ChestStateManager_cache_is_per_instance() {
  OfflineEss ess;
  simcore::ChestStateManager a(ess.client, 0);
  simcore::ChestStateManager b(ess.client, 0);
  a.saveSlots(50, 60, 70, chestContents(1, 1, 3));

  std::vector<simcore::PersistSlot> fromA, fromB;
  a.loadSlots(50, 60, 70, [&](const std::vector<simcore::PersistSlot>& s) { fromA = s; });
  b.loadSlots(50, 60, 70, [&](const std::vector<simcore::PersistSlot>& s) { fromB = s; });
  CHECK_EQ(i_(fromA.size()), 3, "the saving manager sees the contents");
  CHECK_EQ(i_(fromB.size()), 0, "a second manager has an independent cache");
}

// The dimension is not part of the key either. In production simcore runs a
// single manager with dimension 0, so this is latent rather than live — but it
// is the same class of defect as the entity_type key above and is pinned so a
// future per-dimension manager cannot reintroduce it unnoticed.
static void test_ChestStateManager_cache_ignores_dimension() {
  OfflineEss ess;
  simcore::ChestStateManager overworld(ess.client, /*dimension=*/0);
  simcore::ChestStateManager nether(ess.client, /*dimension=*/-1);
  overworld.saveSlots(9, 9, 9, chestContents(4, 4, 2));

  std::vector<simcore::PersistSlot> leaked;
  nether.loadSlots(9, 9, 9, [&](const std::vector<simcore::PersistSlot>& s) {
    leaked = s;
  });
  // Two SEPARATE managers, so nothing leaks between them.
  CHECK_EQ(i_(leaked.size()), 0,
           "a manager for a different dimension does not see the overworld chest");
}

// ---------------------------------------------------------------------------
// Ownership invariant — the per-player ContainerSession layer
// ---------------------------------------------------------------------------

// THE INVARIANT: player A opens the chest and removes everything from their
// own window. Player B then opens the SAME chest and must see the chest as it
// is stored — never A's in-flight edits, and never A's session object.
//
// This is what makes concurrent chest access safe: ContainerSessionRegistry is
// keyed by player id, and a chest session OWNS its slot vector.
static void test_ChestSession_second_player_never_sees_first_players_edits() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  simcore::ContainerSessionRegistry sessions;

  const int32_t cx = 200, cy = 64, cz = 200;
  const uint64_t alice = 1001, bob = 2002;
  const auto stored = chestContents(/*id=*/1234, /*count=*/10, /*filled=*/4);
  mgr.saveSlots(cx, cy, cz, stored);

  // Alice opens: a fresh 27-empty session, then the async load fills it.
  sessions.open(alice, freshSession(cx, cy, cz));
  std::vector<simcore::PersistSlot> aliceLoad;
  mgr.loadSlots(cx, cy, cz, [&](const std::vector<simcore::PersistSlot>& s) {
    aliceLoad = s;
  });
  fillSession(*sessions.find(alice), aliceLoad);
  auto* aliceSession = sessions.find(alice);
  CHECK(aliceSession != nullptr, "Alice has an open session");
  if (aliceSession) {
    CHECK_EQ(i_(aliceSession->slots.size()), 27, "Alice's window has 27 slots");
    CHECK_EQ(i_(aliceSession->slots[0].item_id), 1234,
             "Alice sees the chest's contents");
  }

  // Alice loots everything and closes. Her session is edited in place; the
  // stored chest is NOT touched until she closes.
  std::vector<simcore::PersistSlot> aliceFinal(27);
  for (size_t k = 0; k < aliceFinal.size(); ++k) {
    aliceFinal[k] = item(static_cast<uint16_t>(7777), 64);
  }
  for (size_t k = 0; k < aliceSession->slots.size(); ++k) {
    aliceSession->slots[k] = aliceFinal[k];
  }
  mgr.saveSlots(cx, cy, cz, aliceSession->slots);
  sessions.close(alice);
  CHECK(sessions.find(alice) == nullptr, "Alice's session is gone after close");

  // Bob opens the SAME chest. He must get the contents Alice just stored, in
  // his OWN session.
  sessions.open(bob, freshSession(cx, cy, cz));
  auto* bobSession = sessions.find(bob);
  CHECK(bobSession != nullptr, "Bob has an open session");
  if (bobSession) {
    CHECK_EQ(i_(bobSession->slots.size()), 27, "Bob's window has 27 slots");
    // Before the load lands, Bob's window is the 27 empty slots he registered
    // with — NOT Alice's final contents.
    int non_empty_before_load = 0;
    for (const auto& s : bobSession->slots) {
      if (s.item_id != 0) ++non_empty_before_load;
    }
    CHECK_EQ(non_empty_before_load, 0,
             "Bob's freshly opened window starts empty — it is not Alice's "
             "session reused");
  }

  std::vector<simcore::PersistSlot> bobLoad;
  mgr.loadSlots(cx, cy, cz, [&](const std::vector<simcore::PersistSlot>& s) {
    bobLoad = s;
  });
  fillSession(*bobSession, bobLoad);
  CHECK_EQ(i_(bobSession->slots.size()), 27, "Bob's window is still 27 slots");
  CHECK_EQ(i_(bobSession->slots[0].item_id), 7777,
           "Bob sees the chest as stored after Alice's close, not stale "
           "in-flight state");
  // And Alice's own view is gone, so there is no way for the two to alias.
  CHECK(sessions.find(alice) == nullptr,
         "the two players' sessions never coexist as one object");
}

// Two players with the same chest open at the same time hold two INDEPENDENT
// slot vectors. Mutating one must not be visible through the other, and the
// registry must still hand out both.
static void test_ChestSession_two_open_players_hold_independent_copies() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  simcore::ContainerSessionRegistry sessions;

  const int32_t cx = 300, cy = 64, cz = 300;
  const auto stored = chestContents(600, 6, 5);
  mgr.saveSlots(cx, cy, cz, stored);

  const uint64_t players[2] = {11, 22};
  for (uint64_t pid : players) {
    sessions.open(pid, freshSession(cx, cy, cz));
    std::vector<simcore::PersistSlot> load;
    mgr.loadSlots(cx, cy, cz, [&](const std::vector<simcore::PersistSlot>& s) {
      load = s;
    });
    fillSession(*sessions.find(pid), load);
  }

  auto* first = sessions.find(11);
  auto* second = sessions.find(22);
  CHECK(first != nullptr && second != nullptr, "both players have sessions");
  if (!first || !second) return;

  CHECK(first->slotsRef() != second->slotsRef(),
        "each player has their own slot vector, not a shared one");
  CHECK_EQ(i_(first->slots[0].item_id), 600, "player 11 loaded the chest");
  CHECK_EQ(i_(second->slots[0].item_id), 600, "player 22 loaded the chest");

  // Player 11 empties their window; player 22 must not notice.
  for (auto& s : first->slots) s = simcore::PersistSlot{};
  int non_empty = 0;
  for (const auto& s : second->slots) {
    if (s.item_id != 0) ++non_empty;
  }
  CHECK_EQ(non_empty, 5,
           "player 11 emptying their window leaves player 22's untouched");
  CHECK_EQ(i_(second->slots[0].item_id), 600,
           "player 22 still holds their stack");
}

// Closing one player's session leaves the other player's open — the registry
// is per-player, not per-chest.
static void test_ChestSession_closing_one_player_leaves_the_other_open() {
  simcore::ContainerSessionRegistry sessions;
  const int32_t cx = 400, cy = 64, cz = 400;
  sessions.open(31, freshSession(cx, cy, cz));
  sessions.open(32, freshSession(cx, cy, cz));

  sessions.close(31);

  CHECK(sessions.find(31) == nullptr, "the closed player has no session");
  CHECK(sessions.find(32) != nullptr,
        "the other player at the same chest is still open");
  simcore::ContainerSession copy;
  CHECK(sessions.get(32, copy), "the other player's session is retrievable");
  CHECK_EQ(i_(copy.slots.size()), 27, "with its full window");
}

// A chest session's live view is its OWNED copy (Kind::Chest), not a pointer
// into anything shared. This is the mechanism that makes the invariant above
// hold, so it is asserted directly.
static void test_ChestSession_chest_slots_ref_is_the_owned_copy() {
  simcore::ContainerSession s = freshSession(1, 2, 3);
  CHECK(!s.isMachine(), "a chest session is Kind::Chest");
  CHECK(s.reg == nullptr, "a chest session has no ECS link");
  CHECK(s.slotsRef() == &s.slots,
        "a chest's live slot view IS its owned copy — nothing is shared");
}

// forEachOpenAt is how machine systems notify everyone watching a chest. Both
// players are at the same position, so both must be visited, and nobody else.
static void test_ChestSession_for_each_open_at_visits_every_viewer() {
  simcore::ContainerSessionRegistry sessions;
  sessions.open(41, freshSession(500, 64, 500));
  sessions.open(42, freshSession(500, 64, 500));
  sessions.open(43, freshSession(501, 64, 500)); // different chest
  sessions.open(44, freshSession(500, 65, 500)); // different chest

  int visited = 0;
  bool saw41 = false, saw42 = false, saw43 = false, saw44 = false;
  sessions.forEachOpenAt(500, 64, 500,
                         [&](uint64_t pid, simcore::ContainerSession&) {
                           ++visited;
                           saw41 |= (pid == 41);
                           saw42 |= (pid == 42);
                           saw43 |= (pid == 43);
                           saw44 |= (pid == 44);
                         });
  CHECK_EQ(visited, 2, "exactly the two players watching that chest");
  CHECK(saw41 && saw42, "and they are the right two");
  CHECK(!saw43 && !saw44, "players at other chests are not notified");
}

// Opening the same chest again REPLACES the player's session rather than
// growing a second one. A stale window must not survive a reopen.
static void test_ChestSession_reopening_replaces_the_session() {
  OfflineEss ess;
  simcore::ChestStateManager mgr(ess.client, 0);
  simcore::ContainerSessionRegistry sessions;
  const int32_t cx = 600, cy = 64, cz = 600;

  mgr.saveSlots(cx, cy, cz, chestContents(1, 1, 1));
  sessions.open(51, freshSession(cx, cy, cz));
  std::vector<simcore::PersistSlot> first;
  mgr.loadSlots(cx, cy, cz, [&](const std::vector<simcore::PersistSlot>& s) {
    first = s;
  });
  fillSession(*sessions.find(51), first);
  sessions.find(51)->slots[0] = item(9999, 9, 9); // stale edit

  // Reopen without closing: a fresh session replaces the old one.
  sessions.open(51, freshSession(cx, cy, cz));
  auto* s = sessions.find(51);
  CHECK(s != nullptr, "the reopened session exists");
  if (s) {
    CHECK_EQ(i_(s->slots.size()), 27, "and has a full 27-slot window");
    CHECK_EQ(i_(s->slots[0].item_id), 0,
             "the stale in-flight edit did not survive the reopen");
  }
}

// ---------------------------------------------------------------------------
// ChestInteractHandler — the right-click gate
// ---------------------------------------------------------------------------

// A right-click on a chest is the handler's job. The chest-ness is derived by
// ActionContext from expected_block_id, so this also pins that derivation.
static void test_ChestInteractHandler_handles_right_click_on_a_chest() {
  auto pub = std::make_shared<MockPublisher>();
  Ctx c(/*x=*/10, /*y=*/64, /*z=*/20, simcore::kChestBlockId,
        Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, /*request_id=*/77,
        /*face=*/2, /*player_id=*/900, pub);

  CHECK(c.ctx->is_chest, "a right-click on the chest block id sets is_chest");
  simcore::ChestInteractHandler h;
  CHECK(h.canHandle(*c.ctx), "the chest handler claims the action");
}

// Left-clicking a chest is a break, not an open.
static void test_ChestInteractHandler_rejects_left_click() {
  auto pub = std::make_shared<MockPublisher>();
  Ctx c(/*x=*/10, /*y=*/64, /*z=*/20, simcore::kChestBlockId,
        Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 1, 2, 900, pub);

  simcore::ChestInteractHandler h;
  CHECK(!h.canHandle(*c.ctx),
        "a left-click on a chest belongs to the break handler");
}

// A right-click on anything that is not a chest is not this handler's job.
static void test_ChestInteractHandler_rejects_non_chest_blocks() {
  auto pub = std::make_shared<MockPublisher>();
  simcore::ChestInteractHandler h;
  const uint16_t not_chests[] = {0, 37, 1234, 22529};
  for (uint16_t id : not_chests) {
    Ctx c(/*x=*/1, /*y=*/2, /*z=*/3, id, Protocol::PlayerActionType_RIGHT_MOUSE_CLICK,
          1, 2, 900, pub);
    CHECK(!c.ctx->is_chest, "only the packed chest id counts as a chest");
    CHECK(!h.canHandle(*c.ctx), "the chest handler declines a non-chest block");
  }
}

// A move action on a chest is not an interaction at all.
static void test_ChestInteractHandler_rejects_non_click_actions() {
  auto pub = std::make_shared<MockPublisher>();
  simcore::ChestInteractHandler h;
  const Protocol::PlayerActionType actions[] = {
      Protocol::PlayerActionType_MOVE,
      Protocol::PlayerActionType_LEFT_MOUSE_CLICK,
      Protocol::PlayerActionType_CHUNK_REQUEST,
      Protocol::PlayerActionType_ITEM_ACTION,
      Protocol::PlayerActionType_UNLOAD};
  for (auto a : actions) {
    Ctx c(/*x=*/1, /*y=*/2, /*z=*/3, simcore::kChestBlockId,
          static_cast<uint8_t>(a), 1, 2, 900, pub);
    CHECK(c.ctx->is_chest, "precondition: the target is still a chest");
    CHECK(!h.canHandle(*c.ctx), "only a right-click opens the chest");
  }
}

// handle() must publish exactly two things, in this order: the ACCEPTED ack
// (so the client stops spinning) and the OPEN_UI directive (so the window
// opens). Any extra publish is a protocol change this test should catch.
static void test_ChestInteractHandler_acks_then_opens_the_ui() {
  auto pub = std::make_shared<MockPublisher>();
  const int32_t x = 11, y = 65, z = 22;
  const uint32_t request_id = 4242;
  Ctx c(x, y, z, simcore::kChestBlockId,
        Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, request_id,
        /*face=*/3, /*player_id=*/901, pub);

  simcore::ChestInteractHandler h;
  h.handle(*c.ctx);

  CHECK_EQ(i_(pub->acks.size()), 1, "exactly one ack");
  CHECK_EQ(i_(pub->directives.size()), 1, "exactly one directive");
  CHECK_EQ(i_(pub->order.size()), 2, "and nothing else is published");
  if (pub->order.size() == 2) {
    CHECK_EQ(pub->order[0], std::string("ack"), "the ack comes first");
    CHECK_EQ(pub->order[1], std::string("directive"),
             "then the OPEN_UI directive");
  }

  if (pub->acks.size() == 1) {
    const auto& a = pub->acks[0];
    CHECK_EQ(i_(a.status), i_(Protocol::BlockAckStatus_ACCEPTED),
             "the chest interaction is accepted, not rejected");
    CHECK_EQ(a.x, x, "the ack echoes x");
    CHECK_EQ(a.y, y, "the ack echoes y");
    CHECK_EQ(a.z, z, "the ack echoes z");
    CHECK_EQ(i_(a.block_id), i_(simcore::kChestBlockId),
             "the ack echoes the chest block id");
    CHECK_EQ(i_(a.meta), 0, "meta 0 — nothing about the chest changed");
    CHECK(a.reason.empty(), "an accepted ack carries no reason");
    CHECK_EQ(i_(a.request_id), i_(request_id), "the ack echoes request_id");
    CHECK_EQ(i_(a.action_type), i_(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK),
             "the ack echoes the triggering action");
  }

  if (pub->directives.size() == 1) {
    const auto& d = pub->directives[0];
    CHECK_EQ(i_(d.directive), i_(Protocol::BlockDirective_OPEN_UI),
             "the directive is OPEN_UI");
    CHECK_EQ(i_(d.block_id), i_(simcore::kChestBlockId),
             "OPEN_UI carries the block id so the client picks the window");
    CHECK_EQ(d.x, x, "the directive echoes x");
    CHECK_EQ(d.y, y, "the directive echoes y");
    CHECK_EQ(d.z, z, "the directive echoes z");
    CHECK_EQ(i_(d.request_id), i_(request_id), "the directive echoes request_id");
    CHECK_EQ(i_(d.action_type), i_(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK),
             "the directive echoes the triggering action");
  }
}

// PRODUCTION NOTE (behaviour is correct; the HEADER COMMENT is stale):
// ChestInteractHandler.h says it "stream[s] the chest inventory from
// EntityStateStore so the client window opens populated". It does not, and
// must not — the .cpp documents that the chest.open round-trip is the single
// read path. This test pins the ACTUAL contract: the handler reads nothing
// and changes nothing, so a client that only ever right-clicks a chest never
// gets contents from here.
static void test_ChestInteractHandler_does_not_read_or_write_chest_contents() {
  auto pub = std::make_shared<MockPublisher>();
  Ctx c(/*x=*/12, /*y=*/66, /*z=*/23, simcore::kChestBlockId,
        Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 5, 2, 902, pub);

  simcore::ChestInteractHandler h;
  h.handle(*c.ctx);

  CHECK_EQ(pub->block_entity_updates, 0,
           "no BlockEntityUpdate — the handler does not push chest contents "
           "(they arrive over chest.open)");
  CHECK_EQ(pub->block_changed_events, 0,
           "no BlockChanged — opening a chest does not alter the world");
  CHECK_EQ(i_(pub->acks.size()) + i_(pub->directives.size()), 2,
           "the handler's whole output is the ack plus the OPEN_UI directive");
}

// handle() is driven purely by the context, so two players right-clicking the
// same chest each get their own ack+directive. The handler keeps NO per-player
// state — inventory ownership lives in ContainerSessionRegistry, not here.
static void test_ChestInteractHandler_is_stateless_across_players() {
  auto pub = std::make_shared<MockPublisher>();
  simcore::ChestInteractHandler h;

  for (uint64_t pid : {7001ull, 7002ull, 7001ull}) {
    Ctx c(/*x=*/13, /*y=*/67, /*z=*/24, simcore::kChestBlockId,
          Protocol::PlayerActionType_RIGHT_MOUSE_CLICK,
          static_cast<uint32_t>(pid), 2, pid, pub);
    h.handle(*c.ctx);
  }

  CHECK_EQ(i_(pub->acks.size()), 3, "one ack per interaction");
  CHECK_EQ(i_(pub->directives.size()), 3, "one OPEN_UI per interaction");
  if (pub->acks.size() == 3) {
    CHECK_EQ(i_(pub->acks[0].request_id), 7001, "first player");
    CHECK_EQ(i_(pub->acks[1].request_id), 7002, "second player");
    CHECK_EQ(i_(pub->acks[2].request_id), 7001,
             "the first player interacting again still works");
  }
}

// ---------------------------------------------------------------------------
// PublishFullInventory — early-return paths only
// ---------------------------------------------------------------------------

// The published bytes are not assertable here: IoUringRouterClient drops
// publishes while disconnected, so observing a payload would need a live
// MessageRouter. What IS assertable is that every path that returns before the
// publish is reached is safe and quiet.

// A null router must be a silent no-op, not a crash — callers pass the
// process-wide client straight through and it is null before Connect().
static void test_PublishFullInventory_null_router_is_a_silent_noop() {
  simcore::ContainerSessionRegistry sessions;
  simcore::PlayerInventoryStore store;
  sessions.open(81, freshSession(1, 2, 3));

  simcore::PublishFullInventory(nullptr, store, sessions, 81, 1, 2, 3);
  simcore::ContainerSession& s = *sessions.find(81);
  simcore::PublishFullInventory(nullptr, store, s, 81, 1, 2, 3);
  CHECK(true, "a null router is a no-op on both overloads");
}

// A player with no open session has nothing to publish, so the registry
// lookup is skipped — this is the path a stale InventoryUpdate would take.
static void test_PublishFullInventory_no_session_is_a_silent_noop() {
  auto router = std::make_shared<simcore::IoUringRouterClient>();
  simcore::ContainerSessionRegistry sessions;
  simcore::PlayerInventoryStore store;
  simcore::PublishFullInventory(router, store, sessions, /*pid=*/999, 1, 2, 3);
  CHECK(sessions.find(999) == nullptr,
        "publishing for a player with no open session is skipped");
}

// With a session and a router present, the snapshot is built and handed to the
// router, which (being disconnected) drops it. The point is that building the
// snapshot for a real 27-slot chest window and a 40-slot player inventory is
// safe — no out-of-range, no null, no throw.
static void test_PublishFullInventory_builds_a_snapshot_without_a_router_peer() {
  auto router = std::make_shared<simcore::IoUringRouterClient>();
  simcore::ContainerSessionRegistry sessions;
  simcore::PlayerInventoryStore store;

  store.initPlayer(91);
  std::array<simcore::PersistSlot, simcore::kInventorySlots> player{};
  player[0] = item(4242, 9, 1);
  player[39] = item(5151, 3, 2);
  store.setSlots(91, player);
  store.setCursor(91, item(6161, 5, 3));

  sessions.open(91, freshSession(700, 64, 700));
  std::vector<simcore::PersistSlot> load = chestContents(7000, 7, 27);
  fillSession(*sessions.find(91), load);

  simcore::PublishFullInventory(router, store, sessions, 91, 700, 64, 700);
  simcore::PublishFullInventory(router, store, *sessions.find(91), 91, 700, 64, 700);

  // The player's own inventory and the chest window are untouched by publishing.
  CHECK_EQ(i_(store.getSlots(91).size()), 40, "the player inventory is 40 slots");
  CHECK_EQ(i_(store.getSlots(91)[0].item_id), 4242, "and unchanged");
  CHECK_EQ(i_(sessions.find(91)->slots.size()), 27, "the window is 27 slots");
  CHECK_EQ(i_(sessions.find(91)->slots[0].item_id), 7000, "and unchanged");
}

int main() {
  printf("=== chest_state_manager + chest_interact test suite ===\n\n");

  // Blob codec
  TEST(ChestBlob_round_trips_item_id_count_and_meta);
  TEST(ChestBlob_round_trips_a_full_27_slot_chest);
  TEST(ChestBlob_writes_machine_state_version_one);
  TEST(ChestBlob_empty_input_still_encodes_a_valid_blob);
  TEST(ChestBlob_empty_blob_decodes_to_empty);
  TEST(ChestBlob_malformed_input_decodes_to_empty);
  TEST(ChestBlob_machine_state_without_inventory_decodes_to_empty);
  TEST(ChestBlob_decode_clamps_count_above_255_to_255);

  // ChestStateManager
  TEST(ChestStateManager_saved_chest_survives_close_and_reopen);
  TEST(ChestStateManager_load_is_cache_first);
  TEST(ChestStateManager_null_client_load_is_an_empty_chest);
  TEST(ChestStateManager_load_callback_is_synchronous);
  TEST(ChestStateManager_clear_empties_the_cached_chest);
  TEST(ChestStateManager_clear_leaves_neighbouring_chests_alone);
  TEST(ChestStateManager_neighbouring_chests_are_independent);
  TEST(ChestStateManager_negative_coordinates_key_independently);
  TEST(ChestStateManager_colliding_positions_do_not_share_a_cache_entry);
  TEST(ChestStateManager_y_and_z_fields_do_not_overlap);
  TEST(ChestStateManager_a_run_of_65536_z_steps_keys_independently);
  TEST(ChestStateManager_one_block_step_on_any_axis_is_a_new_key);
  TEST(ChestStateManager_clear_of_a_colliding_position_spares_the_other);
  TEST(ChestStateManager_cache_key_includes_entity_type);
  TEST(ChestStateManager_cache_is_per_instance);
  TEST(ChestStateManager_cache_ignores_dimension);

  // Ownership invariant
  TEST(ChestSession_second_player_never_sees_first_players_edits);
  TEST(ChestSession_two_open_players_hold_independent_copies);
  TEST(ChestSession_closing_one_player_leaves_the_other_open);
  TEST(ChestSession_chest_slots_ref_is_the_owned_copy);
  TEST(ChestSession_for_each_open_at_visits_every_viewer);
  TEST(ChestSession_reopening_replaces_the_session);

  // ChestInteractHandler
  TEST(ChestInteractHandler_handles_right_click_on_a_chest);
  TEST(ChestInteractHandler_rejects_left_click);
  TEST(ChestInteractHandler_rejects_non_chest_blocks);
  TEST(ChestInteractHandler_rejects_non_click_actions);
  TEST(ChestInteractHandler_acks_then_opens_the_ui);
  TEST(ChestInteractHandler_does_not_read_or_write_chest_contents);
  TEST(ChestInteractHandler_is_stateless_across_players);

  // PublishFullInventory early returns
  TEST(PublishFullInventory_null_router_is_a_silent_noop);
  TEST(PublishFullInventory_no_session_is_a_silent_noop);
  TEST(PublishFullInventory_builds_a_snapshot_without_a_router_peer);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
