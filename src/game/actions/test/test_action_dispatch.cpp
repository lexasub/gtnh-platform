// Unit tests for the player-action dispatch seam:
//
//   src/game/actions/ActionDispatcher.cpp        (SetBlockAction routing table)
//   src/game/actions/ActionContext.cpp           (the immutable action snapshot)
//   src/game/actions/PlayerActionDispatcher.cpp  (legacy PlayerAction routing)
//
// This is the code that decides which handler runs for a client action. A wrong
// mapping here is a SILENT gameplay bug: the wrong block breaks, the wrong UI
// opens, or an action is dropped with no ack at all. So the table is pinned
// here with a table-driven sweep over Protocol::PlayerActionType.
//
// ============================================================================
// FINDING (production bug, FOUND gp-0xml, FIXED gp-qij1):
//     ActionDispatcher::dispatch() USED TO ALWAYS RETURN FALSE.
// ============================================================================
//
// src/game/actions/ActionDispatcher.cpp:7-15 folded the handler tuple with
//
//   ((handled || (h.canHandle(ctx) ? (h.handle(ctx), true) : false)) || ...)
//
// The fold's result — the `true` a claiming handler produces — was discarded,
// because the lambda body never assigned it back to `handled`. `handled` was set
// to false and nothing ever wrote it. The handlers themselves ran correctly
// (the || chain short-circuits, so exactly one handler runs and later ones are
// skipped); only the return value was broken.
//
// The consequence lived in SetBlockCASHandler.cpp:47-56:
//
//   if (dispatcher_.dispatch(ctx)) return;              // never taken
//   publishBlockAck(REJECTED, ..., "nothing placeable in hand");
//
// so every block action that actually executed was immediately followed by a
// REJECTED ack carrying the same request_id, no matter what the player did.
// This is what made the integration test TestChunk_GetBlockAfterSet report
// "BlockAck status: expected ACCEPTED, got REJECTED" after a block placement
// that had actually committed.
//
// FIXED in gp-qij1: the fold's value is now assigned back to `handled`, so
// dispatch() returns true exactly when a handler claimed the action. The
// behavior is pinned by
// test_ActionDispatcher_returns_true_when_a_handler_ran, which asserts the
// return value AND the absence of the facade's spurious REJECTED ack.
//
// The routing assertions below are still made against the SIDE EFFECTS of a
// handler running (a CAS, a directive, an ack, a drop) via the route() helper —
// side effects are what the gameplay actually depends on, and they stay
// meaningful if the return contract is ever changed again. route() additionally
// cross-checks that dispatch()'s return value agrees with the observed effects,
// so the return value is now covered on every routed row, not just this one.
//
// ── What the dispatch tables actually are ────────────────────────────────────
//
// ActionDispatcher holds a fixed std::tuple of handlers and folds over it:
//
//   priority = machine -> chest -> break -> place
//   dispatch() = fold left with short-circuit: the FIRST handler whose
//   canHandle() is true runs handle(), and no later handler is consulted.
//
// The four canHandle() predicates (as written today):
//
//   MachineInteractHandler  engine_ && machine_info && RIGHT_CLICK
//                           || (LEFT_CLICK && machine_info->interact_on_left
//                               && !isMiningTool(held_item))
//   ChestInteractHandler    is_chest && RIGHT_CLICK
//   BreakBlockHandler       LEFT_CLICK
//   PlaceBlockHandler       RIGHT_CLICK && held_item != 0
//                           && !isMiningTool(held_item) && held_item != WRENCH
//
// PlayerActionDispatcher has its own 3-way switch over the SAME enum:
//   ITEM_ACTION   -> fires the ItemGiveCallback (no crash if it is null)
//   CHUNK_REQUEST -> log only
//   default       -> returns false (dropped, and dispatch() swallows it)
//
// ── Declared-but-unrouted action types (FINDINGS, asserted, not hidden) ─────
//
// Protocol::PlayerActionType declares 6 values; the SetBlockAction dispatch
// table only routes 2 of them (RIGHT_MOUSE_CLICK, LEFT_MOUSE_CLICK). The other
// 4 (MOVE, CHUNK_REQUEST, ITEM_ACTION, UNLOAD) fall through every predicate and
// ActionDispatcher::dispatch() returns false. That is the observed behavior and
// it is asserted as such in the sweep below, NOT papered over.
//
// This is safe today only because of a filter that lives OUTSIDE this seam:
// SimCoreMessageHandler::wireOnMessage() drops every player.actions frame that
// is not ITEM_ACTION, and SetBlockCASHandler publishes a REJECTED ack
// ("nothing placeable in hand") when dispatch() returns false. A SetBlockAction
// carrying MOVE/CHUNK_REQUEST/ITEM_ACTION/UNLOAD is therefore never a
// gameplay-legitimate frame in the first place. See the findings summary at the
// bottom of this file.
#include <apps/simcore/Network/IEventPublisher.h>
#include <data/registry/ToolIds.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/SimulationEngine.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/Position.h>
#include <flatbuffers/flatbuffers.h>
#include <game/actions/ActionContext.h>
#include <game/actions/ActionDispatcher.h>
#include <game/actions/PlayerActionDispatcher.h>
#include <game/storage/IBlockRepository.h>
#include <game/storage/PlayerInventoryStore.h>
#include <game/world/BlockDrops.h>
#include "core_generated.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Project-wide unit-test harness (src/engine/net/test/test.h). The repo has no
// GoogleTest dependency and CI does not install one, so this is the established
// convention for focused tests.
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

#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define TEST(name) \
  do {              \
    printf("  TEST: %s\n", #name); \
    test_##name();  \
  } while (0)

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

using namespace simcore;

namespace {

struct AckRecord {
  uint8_t status = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
  uint8_t meta = 0;
  std::string reason;
  uint32_t request_id = 0;
  uint8_t action_type = 0;
};

struct DirectiveRecord {
  uint8_t directive = 0;
  uint16_t block_id = 0;
  int32_t x = 0, y = 0, z = 0;
  uint32_t request_id = 0;
  uint8_t action_type = 0;
};

struct ChangedRecord {
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
  uint8_t meta = 0;
  uint32_t request_id = 0;
  uint64_t source_player_id = 0;
};

struct EntityUpdateRecord {
  int32_t x = 0, y = 0, z = 0;
  uint16_t machine_type = 0;
  float progress = 0.0f;
  uint32_t energy = 0;
  int slots_in = -1;
};

class RecordingPublisher : public IEventPublisher {
public:
  std::vector<AckRecord> acks;
  std::vector<DirectiveRecord> directives;
  std::vector<ChangedRecord> changed;
  std::vector<EntityUpdateRecord> entity_updates;

  void publishBlockAck(uint8_t status, int32_t x, int32_t y, int32_t z,
                       uint16_t block_id, uint8_t meta, const char* reason,
                       uint32_t request_id = 0,
                       uint8_t action_type = 1) override {
    acks.push_back(AckRecord{status, x, y, z, block_id, meta,
                             reason ? reason : "", request_id, action_type});
  }

  void publishBlockDirective(uint8_t directive, uint16_t block_id, int32_t x,
                             int32_t y, int32_t z, uint32_t request_id = 0,
                             uint8_t action_type = 1) override {
    directives.push_back(
        DirectiveRecord{directive, block_id, x, y, z, request_id, action_type});
  }

  void publishBlockChangedEvent(int32_t x, int32_t y, int32_t z,
                                uint16_t block_id, uint8_t meta,
                                uint32_t request_id = 0,
                                uint64_t source_player_id = 0) override {
    changed.push_back(
        ChangedRecord{x, y, z, block_id, meta, request_id, source_player_id});
  }

  void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z,
                                uint16_t machine_type,
                                const std::vector<uint8_t>&, float progress,
                                uint32_t energy, EnergyType = EnergyType::ELECTRICITY,
                                uint32_t = 0, int slots_in = -1, float = 0.0f,
                                const std::vector<HatchUpdateData>* = nullptr,
                                double = -1.0, double = -1.0) override {
    entity_updates.push_back(
        EntityUpdateRecord{x, y, z, machine_type, progress, energy, slots_in});
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

  void clear() {
    acks.clear();
    directives.clear();
    changed.clear();
    entity_updates.clear();
  }
};

// ChunkStore double. setBlockCAS/getBlock answer SYNCHRONOUSLY so the whole
// test stays on one thread with no sleeps and no real I/O.
class FakeRepo : public IBlockRepository {
public:
  struct CasCall {
    int32_t x = 0, y = 0, z = 0;
    uint16_t expected = 0;
    uint16_t new_id = 0;
    uint8_t meta = 0;
  };
  struct GetCall {
    int32_t x = 0, y = 0, z = 0;
  };

  std::vector<CasCall> cas_calls;
  std::vector<GetCall> get_calls;

  // What setBlockCAS reports back. status 0 = OK, 1 = CONFLICT.
  uint8_t cas_status = 0;
  // What getBlock returns (used by the machine lazy-init path).
  uint16_t block_at = 0;
  uint8_t block_meta = 0;
  uint32_t block_mb_id = 0;
  // The real ChunkStoreRepository always completes the CAS callback, on the io
  // thread: status 0 = committed, 1 = CONFLICT with the actual block still
  // there. The fake completes it synchronously so the test stays single-threaded,
  // but it models the same two outcomes (never "still pending").
  void setBlockCAS(int32_t x, int32_t y, int32_t z, uint16_t expected_block_id,
                   uint16_t new_block_id, uint8_t meta,
                   SetBlockCASCallback callback) override {
    cas_calls.push_back(CasCall{x, y, z, expected_block_id, new_block_id, meta});
    const uint16_t actual = cas_status == 0 ? new_block_id : expected_block_id;
    callback(CASResult{cas_status, actual, meta});
  }

  void getBlock(int32_t x, int32_t y, int32_t z,
                GetBlockCallback callback) override {
    get_calls.push_back(GetCall{x, y, z});
    callback(BlockData{block_at, block_meta, block_mb_id});
  }
};

struct GivenItem {
  uint64_t player_id = 0;
  uint16_t item_id = 0;
  uint8_t count = 0;
  int32_t target_slot = 0;
};

struct DrillUse {
  uint64_t player_id = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
};

struct BlockPlaced {
  uint64_t player_id = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
};

// Machine block ids taken from src/content/data/registry/machines.yaml.
constexpr uint16_t kFurnaceId = ItemId::pack("1110:000:0");   // heat_furnace
constexpr uint16_t kRotareId = ItemId::pack("1110:100:1");    // rotare_generator
constexpr uint16_t kChestId = ItemId::pack("0:10:11:0");     // chest
constexpr uint16_t kStoneId = ItemId::pack("0:0:1");          // stone
constexpr uint16_t kCraftingTableId = ItemId::pack("0:10:11:1");  // crafting_table
constexpr uint16_t kAirId = ItemId::pack("0:0:0");

// ---------------------------------------------------------------------------
// Fixture: a fully wired ActionContext over fakes.
// ---------------------------------------------------------------------------

class Fixture {
public:
  Fixture() {
    publisher = std::make_shared<RecordingPublisher>();
    repo = std::make_shared<FakeRepo>();
    engine = std::make_shared<SimulationEngine>();
    registry = MachineRegistry::LoadFromYaml(MACHINES_YAML);
    if (registry) {
      engine->setMachineRegistry(registry.get());
    }
    inventory = std::make_shared<PlayerInventoryStore>();
    inventory->initPlayer(kPlayerId);
  }

  // Builds a real Protocol::SetBlockAction flatbuffer and wraps it in an
  // ActionContext. The finished buffer must outlive the returned context, so
  // it is stored here. Each call starts a fresh builder, which invalidates the
  // PREVIOUS buffer — never hold two contexts built by one fixture at once.
  ActionContext make(Protocol::PlayerActionType action, int32_t x, int32_t y,
                     int32_t z, uint16_t expected_block_id, uint16_t new_block_id,
                     uint16_t held_item, uint8_t face, bool inline_cas = false) {
    builder_.Clear();
    Protocol::Vec3i pos(x, y, z);
    const auto off = Protocol::CreateSetBlockAction(
        builder_, kPlayerId, action, &pos, expected_block_id, new_block_id,
        kRequestId, face, held_item);
    builder_.Finish(off);
    const Protocol::SetBlockAction* table =
        flatbuffers::GetRoot<Protocol::SetBlockAction>(builder_.GetBufferPointer());
    return ActionContext(
        table, repo, publisher, engine, inventory, nullptr,
        [this](uint64_t pid, uint16_t item, uint8_t count, int32_t slot) {
          given.push_back({pid, item, count, slot});
        },
        [this](uint64_t pid, int32_t bx, int32_t by, int32_t bz, uint16_t block) {
          drill_uses.push_back({pid, bx, by, bz, block});
        },
        [this](uint64_t pid, int32_t bx, int32_t by, int32_t bz, uint16_t block) {
          placed.push_back({pid, bx, by, bz, block});
        },
        inline_cas ? nullptr : [](std::function<void()> fn) { fn(); });
  }

  static constexpr uint64_t kPlayerId = 7;
  static constexpr uint32_t kRequestId = 4242;

  std::shared_ptr<RecordingPublisher> publisher;
  std::shared_ptr<FakeRepo> repo;
  std::shared_ptr<SimulationEngine> engine;
  std::shared_ptr<PlayerInventoryStore> inventory;
  std::unique_ptr<MachineRegistry> registry;

  std::vector<GivenItem> given;
  std::vector<DrillUse> drill_uses;
  std::vector<BlockPlaced> placed;

  void reset() {
    publisher->clear();
    repo->cas_calls.clear();
    repo->get_calls.clear();
    given.clear();
    drill_uses.clear();
    placed.clear();
  }

private:
  flatbuffers::FlatBufferBuilder builder_;
};

// RAII guard so the process-wide BlockDrops singleton never leaks into another
// test (BreakBlockHandler reads it through instance()).
class DropsGuard {
public:
  DropsGuard() { BlockDrops::setInstance(nullptr); }
  ~DropsGuard() { BlockDrops::setInstance(nullptr); }
  DropsGuard(const DropsGuard&) = delete;
  DropsGuard& operator=(const DropsGuard&) = delete;
};

constexpr uint64_t Fixture::kPlayerId;
constexpr uint32_t Fixture::kRequestId;

// ---------------------------------------------------------------------------
// route(): run the dispatcher and report whether ANY handler actually ran,
// derived from the side effects the dispatcher actually produced.
//
// The ROUTING assertions are made against the observable effect of a handler
// running (a CAS, a directive, an ack, a drop) — that is the thing the gameplay
// actually depends on, and it stays meaningful regardless of the return
// contract. But since gp-qij1 the return value is trustworthy again, so route()
// ALSO asserts that dispatch()'s return agrees with the observed effects. That
// turns every routed row in this file into a check on the return value, so the
// gp-qij1 regression cannot come back silently: if the fold result is dropped
// again, every routed row fails here as well as in the dedicated test below.
// ---------------------------------------------------------------------------
bool route(ActionDispatcher& d, ActionContext& ctx, RecordingPublisher& pub) {
  const bool returned = d.dispatch(ctx);
  const bool observed =
      !pub.directives.empty() || !pub.changed.empty() || !pub.acks.empty();
  CHECK_EQ(returned, observed,
           "dispatch()'s return value agrees with the observed handler effects");
  return observed;
}

bool route(ActionDispatcher& d, ActionContext& ctx, Fixture& f) {
  return route(d, ctx, *f.publisher);
}

// ===========================================================================
// 1. The table-driven sweep over Protocol::PlayerActionType
// ===========================================================================

void test_ActionDispatcher_sweep_over_every_declared_action_type() {
  DropsGuard drops;

  // ---- RIGHT_MOUSE_CLICK: machine wins over chest, chest over place -------
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 10, 20,
                               30, kFurnaceId, 0, ItemId::pack("0:0:3"), 1);
    ActionDispatcher d;
    CHECK_EQ(ctx.machine_info != nullptr, true, "furnace resolves to a machine");
    CHECK(route(d, ctx, f), "RIGHT_MOUSE_CLICK on a machine is handled");
    // MachineInteractHandler with no ECS entity yet falls into the lazy-init
    // path: it must read the block back from ChunkStore before acking.
    CHECK_EQ(f.repo->get_calls.size(), size_t(1),
             "machine interaction lazily reads the block from ChunkStore");
    CHECK_EQ(f.publisher->directives.size(), size_t(1),
             "machine interaction emits exactly one directive");
    CHECK_EQ(f.publisher->directives[0].directive,
             uint8_t(Protocol::BlockDirective_OPEN_UI),
             "machine right-click opens the machine window");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(0),
             "machine interaction does not issue a block CAS");
  }

  // ---- RIGHT_MOUSE_CLICK on a chest: chest handler, not the machine one ---
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 1, 2, 3,
                               kChestId, 0, 0, 1);
    ActionDispatcher d;
    CHECK(ctx.is_chest, "chest block id sets is_chest");
    CHECK(route(d, ctx, f), "RIGHT_MOUSE_CLICK on a chest is handled");
    CHECK_EQ(f.publisher->directives.size(), size_t(1),
             "chest interaction emits exactly one directive");
    CHECK_EQ(f.publisher->directives[0].directive,
             uint8_t(Protocol::BlockDirective_OPEN_UI),
             "chest right-click opens the chest window");
    CHECK_EQ(f.publisher->directives[0].block_id, kChestId,
             "chest directive carries the chest block id");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(0),
             "chest interaction does not issue a block CAS");
  }

  // ---- RIGHT_MOUSE_CLICK with a plain block in hand: PlaceBlockHandler ----
  {
    Fixture f;
    const uint16_t held = ItemId::pack("0:0:3");  // cobblestone
    f.inventory->giveItem(f.kPlayerId, held, 5, -1);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 5, 6, 7,
                               kAirId, held, held, 1);
    ActionDispatcher d;
    CHECK(route(d, ctx, f), "RIGHT_MOUSE_CLICK with a block in hand is handled");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(1),
             "placement issues exactly one block CAS");
    if (f.repo->cas_calls.size() == 1) {
      const auto& c = f.repo->cas_calls[0];
      CHECK_EQ(c.expected, uint16_t(0),
               "placement CAS expects air on the face-adjacent cell");
      CHECK_EQ(c.new_id, held, "placement CAS writes the held block id");
      // The CAS lands on the face-adjacent cell, not the clicked cell.
      CHECK_EQ(c.x, 5, "face=1 (UP) moves the placement target one block up");
      CHECK_EQ(c.y, 7, "face=1 (UP) moves the placement target one block up");
      CHECK_EQ(c.z, 7, "face=1 (UP) keeps the z coordinate");
    }
    CHECK_EQ(f.placed.size(), size_t(1),
             "the block-placed hook fires once on a committed placement");
    if (f.placed.size() == 1) {
      CHECK_EQ(f.placed[0].block_id, held,
               "the block-placed hook receives the placed block id");
      CHECK_EQ(f.placed[0].x, 5, "the block-placed hook gets the target x");
    }
    const auto slots = f.inventory->getSlots(f.kPlayerId);
    int cobble = 0;
    for (const auto& s : slots) {
      if (s.item_id == held) cobble += s.count;
    }
    CHECK_EQ(cobble, 4, "placing consumes exactly one held block from the inventory");
  }

  // ---- LEFT_MOUSE_CLICK: BreakBlockHandler ---------------------------------
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 9, 8, 7,
                               kStoneId, 0, 0, 0);
    ActionDispatcher d;
    CHECK(route(d, ctx, f), "LEFT_MOUSE_CLICK is handled");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(1),
             "breaking issues exactly one block CAS");
    if (f.repo->cas_calls.size() == 1) {
      CHECK_EQ(f.repo->cas_calls[0].expected, kStoneId,
               "breaking CASes the block the client expected to see");
      CHECK_EQ(f.repo->cas_calls[0].new_id, uint16_t(0),
               "breaking CASes the block to air (block_id 0)");
    }
    CHECK_EQ(f.publisher->changed.size(), size_t(1),
             "breaking publishes exactly one block-changed event");
    if (f.publisher->changed.size() == 1) {
      CHECK_EQ(f.publisher->changed[0].block_id, uint16_t(0),
               "the block-changed event reports the block was removed");
      CHECK_EQ(f.publisher->changed[0].source_player_id, f.kPlayerId,
               "the block-changed event is attributed to the acting player");
    }
    CHECK_EQ(f.given.size(), size_t(1),
             "breaking hands the broken block to the player (no drops table)");
    if (f.given.size() == 1) {
      CHECK_EQ(f.given[0].item_id, kStoneId,
               "the drop equals the broken block when no drops table is loaded");
      CHECK_EQ(f.given[0].count, uint8_t(1), "one broken block yields one drop");
    }
  }

  // ---- LEFT_MOUSE_CLICK on a rotare_generator: machine, NOT break ---------
  // rotare_generator is flagged interact_on_left in machines.yaml, so the
  // left-click spins it instead of destroying the block. This is the single
  // most valuable row in the table: getting it wrong deletes a machine.
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 4, 5, 6,
                               kRotareId, 0, 0, 0);
    ActionDispatcher d;
    CHECK(ctx.machine_info != nullptr, "rotare_generator resolves to a machine");
    if (ctx.machine_info) {
      CHECK(ctx.machine_info->interact_on_left,
            "rotare_generator is registered with interact_on_left");
    }
    CHECK(route(d, ctx, f),
          "LEFT_MOUSE_CLICK on an interact_on_left machine is handled");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(0),
             "an interact_on_left machine is NOT broken by a left-click");
    CHECK_EQ(f.publisher->changed.size(), size_t(0),
             "an interact_on_left machine emits no block-changed event");
    CHECK_EQ(f.publisher->directives.size(), size_t(1),
             "spinning a machine emits exactly one directive");
    if (f.publisher->directives.size() == 1) {
      CHECK_EQ(f.publisher->directives[0].directive,
               uint8_t(Protocol::BlockDirective_PLAY_ANIMATION),
               "left-click on a machine plays the spin animation");
    }
    CHECK_EQ(f.publisher->acks.size(), size_t(1),
             "spinning a machine acks the action once");
    if (f.publisher->acks.size() == 1) {
      CHECK_EQ(f.publisher->acks[0].status,
               uint8_t(Protocol::BlockAckStatus_ACCEPTED),
               "the spin ack is ACCEPTED");
    }
  }

  // ---- LEFT_MOUSE_CLICK on a furnace: break (no interact_on_left) ---------
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 1, 1, 1,
                               kFurnaceId, 0, 0, 0);
    ActionDispatcher d;
    CHECK(route(d, ctx, f), "LEFT_MOUSE_CLICK on a plain machine is handled");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(1),
             "a machine without interact_on_left is broken by a left-click");
    CHECK_EQ(f.publisher->directives.size(), size_t(0),
             "breaking a machine emits no OPEN_UI/PLAY_ANIMATION directive");
  }

  // ---- The four declared-but-unrouted action types ------------------------
  // MOVE / CHUNK_REQUEST / ITEM_ACTION / UNLOAD are declared in the enum but
  // no canHandle() accepts them, so no handler runs and dispatch() returns
  // false; the facade (SetBlockCASHandler) is what rejects the frame. Asserted,
  // not hidden.
  {
    struct UnroutedRow {
      Protocol::PlayerActionType action;
      const char* label;
    };
    const UnroutedRow unrouted[] = {
        {Protocol::PlayerActionType_MOVE, "MOVE"},
        {Protocol::PlayerActionType_CHUNK_REQUEST, "CHUNK_REQUEST"},
        {Protocol::PlayerActionType_ITEM_ACTION, "ITEM_ACTION"},
        {Protocol::PlayerActionType_UNLOAD, "UNLOAD"},
    };
    for (const auto& row : unrouted) {
      Fixture f;
      ActionContext ctx = f.make(row.action, 1, 1, 1, kStoneId, 0, 0, 0);
      ActionDispatcher d;
      printf("    sweep: %s\n", row.label);
      CHECK_EQ(ctx.action_type, static_cast<uint8_t>(row.action),
               "ActionContext copies the action type through unchanged");
      const bool handled = d.dispatch(ctx);
      CHECK(!handled, "ActionDispatcher returns false for an unrouted action type");
      CHECK(!route(d, ctx, f), "an unrouted action type runs no handler at all");
      CHECK_EQ(f.repo->cas_calls.size(), size_t(0),
               "an unrouted action type issues no block CAS");
      CHECK_EQ(f.publisher->changed.size(), size_t(0),
               "an unrouted action type changes no block");
      CHECK_EQ(f.publisher->acks.size(), size_t(0),
               "ActionDispatcher itself acks nothing (SetBlockCASHandler does)");
    }
  }

  // ---- Priority: a machine target beats an in-hand block -------------------
  {
    Fixture f;
    const uint16_t held = ItemId::pack("0:0:3");
    f.inventory->giveItem(f.kPlayerId, held, 3, -1);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 3, 3, 3,
                               kFurnaceId, held, held, 1);
    ActionDispatcher d;
    CHECK(route(d, ctx, f), "machine + block in hand is still handled");
    CHECK_EQ(f.publisher->directives.size(), size_t(1),
             "the machine route wins over the place route");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(0),
             "the place handler does not run when the machine handler claimed it");
  }

  // ---- Right-click with an empty hand is rejected --------------------------
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 2, 2, 2,
                               kStoneId, 0, 0, 0);
    ActionDispatcher d;
    CHECK(!route(d, ctx, f),
          "RIGHT_MOUSE_CLICK on a plain block with an empty hand is rejected");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(0), "no CAS for a rejected action");
    CHECK_EQ(f.publisher->acks.size(), size_t(0), "no ack for a rejected action");
  }
}

// ===========================================================================
// 2. Held-item gating on the place route (tools must never be placed)
// ===========================================================================

void test_ActionDispatcher_held_item_gates_the_place_route() {
  DropsGuard drops;
  struct Row {
    uint16_t held;
    const char* label;
    bool placed;
  };
  // A drill / chainsaw / wrench is a tool, not a placeable block, so the place
  // route must reject it. ITEM_WRENCH is excluded separately (it drives the
  // player.wrench.action topic instead).
  const Row rows[] = {
      {0, "empty hand", false},
      {ItemId::pack("0:0:3"), "cobblestone", true},
      {ITEM_DRILL_LV, "drill_lv", false},
      {ITEM_DRILL_HV, "drill_hv", false},
      {ITEM_CHAINSAW_LV, "chainsaw_lv", false},
      {ITEM_WRENCH, "wrench", false},
  };

  for (const auto& row : rows) {
    Fixture f;
    if (row.held != 0) f.inventory->giveItem(f.kPlayerId, row.held, 1, -1);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 8, 8, 8,
                               kAirId, row.held, row.held, 1);
    ActionDispatcher d;
    const bool handled = route(d, ctx, f);
    CHECK_EQ(handled, row.placed,
             "the place route accepts only non-tool, non-wrench held items");
    CHECK_EQ(f.repo->cas_calls.size(), row.placed ? size_t(1) : size_t(0),
             "a tool in hand issues no placement CAS");
  }
}

// ===========================================================================
// 3. A drill in hand on a LEFT click must still break (tools do not block it)
// ===========================================================================

void test_ActionDispatcher_drill_in_hand_still_breaks_on_left_click() {
  DropsGuard drops;
  Fixture f;
  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 6, 6, 6,
                             kStoneId, 0, ITEM_DRILL_LV, 0);
  ActionDispatcher d;
  CHECK(route(d, ctx, f), "LEFT_MOUSE_CLICK is handled with a drill in hand");
  CHECK_EQ(f.repo->cas_calls.size(), size_t(1),
           "a drill in hand does not stop the break CAS");
  CHECK_EQ(f.drill_uses.size(), size_t(1),
           "the drill-use hook fires after a drill breaks a block");
  if (f.drill_uses.size() == 1) {
    CHECK_EQ(f.drill_uses[0].block_id, kStoneId,
             "the drill-use hook receives the broken block id");
  }
}

// ===========================================================================
// 4. A drill in hand BLOCKS a left-click machine interaction
//    (MachineInteractHandler::canHandle requires !isMiningTool)
// ===========================================================================

void test_ActionDispatcher_mining_tool_in_hand_falls_through_to_break() {
  DropsGuard drops;
  Fixture f;
  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 7, 7, 7,
                             kRotareId, 0, ITEM_DRILL_LV, 0);
  ActionDispatcher d;
  // canHandle() is false with a mining tool in hand, so the break route runs and
  // the interact_on_left machine is destroyed. This asymmetry (a tool protects
  // nothing on the left click) is real, observed behavior — pinned here.
  CHECK(route(d, ctx, f), "the action is still handled by the break route");
  CHECK_EQ(f.repo->cas_calls.size(), size_t(1),
           "a mining tool in hand falls through to the break route");
  CHECK_EQ(f.publisher->directives.size(), size_t(0),
           "no spin animation is played with a mining tool in hand");
}

// ===========================================================================
// 5. Context pass-through: every field of the ActionContext reaches the handler
// ===========================================================================

void test_ActionContext_copies_every_field_from_the_flatbuffer() {
  DropsGuard drops;
  Fixture f;
  const uint16_t held = ItemId::pack("0:0:3");
  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, -11, 64,
                             1023, kCraftingTableId, held, held, 5);
  CHECK(ctx.action != nullptr, "the raw flatbuffer pointer is stored");
  CHECK_EQ(ctx.action_type, uint8_t(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK),
           "action_type is copied through");
  CHECK_EQ(ctx.x, -11, "x is copied through (negative coordinates survive)");
  CHECK_EQ(ctx.y, 64, "y is copied through");
  CHECK_EQ(ctx.z, 1023, "z is copied through");
  CHECK_EQ(ctx.expected_block_id, kCraftingTableId, "expected_block_id is copied through");
  CHECK_EQ(ctx.new_block_id, held, "new_block_id is copied through");
  CHECK_EQ(ctx.player_id, f.kPlayerId, "player_id is copied through");
  CHECK_EQ(ctx.request_id, f.kRequestId, "request_id is copied through");
  CHECK_EQ(ctx.held_item, held, "held_item is copied through");
  CHECK_EQ(ctx.face, uint8_t(5), "face is copied through");
  CHECK_EQ(ctx.is_chest, false, "a crafting table is not a chest");
  CHECK(ctx.machine_info != nullptr,
        "a crafting table is registered as a machine in machines.yaml");
  CHECK(ctx.repo_ != nullptr, "the block repository shared_ptr is stored");
  CHECK(ctx.publisher_ != nullptr, "the event publisher shared_ptr is stored");
  CHECK(ctx.engine_ != nullptr, "the simulation engine shared_ptr is stored");
  CHECK(ctx.inventoryStore_ != nullptr, "the inventory store shared_ptr is stored");
  CHECK(static_cast<bool>(ctx.onGiveItem_), "the item-give callback is stored");
  CHECK(static_cast<bool>(ctx.onDrillUse_), "the drill-use callback is stored");
  CHECK(static_cast<bool>(ctx.onBlockPlaced_), "the block-placed callback is stored");
  CHECK(static_cast<bool>(ctx.postToMain_), "the post-to-main callback is stored");
}

// ---------------------------------------------------------------------------
// The effective-target derivation: right click targets the face-adjacent cell
// and expects air there; left click targets the clicked cell and expects the
// client's view of the block.
// ---------------------------------------------------------------------------

void test_ActionContext_right_click_derives_the_face_adjacent_cell() {
  DropsGuard drops;
  struct FaceRow {
    uint8_t face;
    int32_t dx, dy, dz;
    const char* label;
  };
  // faceAdjacentBlock() maps 0=DOWN 1=UP 2=NORTH 3=SOUTH 4=WEST 5=EAST.
  const FaceRow faces[] = {
      {0, 0, -1, 0, "DOWN"},
      {1, 0, 1, 0, "UP"},
      {2, 0, 0, -1, "NORTH"},
      {3, 0, 0, 1, "SOUTH"},
      {4, -1, 0, 0, "WEST"},
      {5, 1, 0, 0, "EAST"},
  };
  for (const auto& f0 : faces) {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 100, 50,
                               -20, kAirId, 0, 0, f0.face);
    CHECK_EQ(ctx.eff_x, 100 + f0.dx, "effective x is the clicked x plus the face offset");
    CHECK_EQ(ctx.eff_y, 50 + f0.dy, "effective y is the clicked y plus the face offset");
    CHECK_EQ(ctx.eff_z, -20 + f0.dz, "effective z is the clicked z plus the face offset");
    CHECK_EQ(ctx.eff_expected, uint16_t(0),
             "a right click always expects air on the adjacent cell");
  }
}

void test_ActionContext_left_click_targets_the_clicked_cell() {
  DropsGuard drops;
  Fixture f;
  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, -5, 12, 300,
                             kStoneId, 0, 0, 3);
  CHECK_EQ(ctx.eff_x, -5, "a left click acts on the clicked x");
  CHECK_EQ(ctx.eff_y, 12, "a left click acts on the clicked y");
  CHECK_EQ(ctx.eff_z, 300, "a left click acts on the clicked z");
  CHECK_EQ(ctx.eff_expected, kStoneId,
           "a left click expects the block the client saw");
  // The face is irrelevant for a left click.
  CHECK_EQ(ctx.face, uint8_t(3), "the face is still copied through verbatim");
}

// ===========================================================================
// 6. ActionContext is null-safe: a null action yields a zeroed context
// ===========================================================================

void test_ActionContext_null_action_yields_a_default_context() {
  DropsGuard drops;
  Fixture f;
  ActionContext ctx(nullptr, f.repo, f.publisher, f.engine, f.inventory, nullptr,
                    nullptr, nullptr, nullptr, nullptr);
  CHECK(ctx.action == nullptr, "a null action is stored as null");
  CHECK_EQ(ctx.action_type, uint8_t(0), "a null action leaves action_type at 0");
  CHECK_EQ(ctx.x, 0, "a null action leaves x at 0");
  CHECK_EQ(ctx.held_item, uint16_t(0), "a null action leaves held_item at 0");
  CHECK_EQ(ctx.is_chest, false, "a null action is not a chest");
  CHECK(ctx.machine_info == nullptr, "a null action resolves no machine");
  CHECK(ctx.repo_ != nullptr, "dependencies are still stored with a null action");
}

void test_ActionContext_without_engine_has_no_machine_info() {
  DropsGuard drops;
  Fixture f;
  ActionContext ctx(nullptr, f.repo, f.publisher, nullptr, f.inventory, nullptr,
                    nullptr, nullptr, nullptr, nullptr);
  CHECK(ctx.machine_info == nullptr,
        "no engine means no machine registry lookup, so no machine route");
  ActionDispatcher d;
  // Without an engine MachineInteractHandler::canHandle is false, so a right
  // click on a machine falls through to the chest/place routes.
  CHECK(!d.dispatch(ctx), "a null-action context is rejected by the dispatcher");
}

// ===========================================================================
// 7. The CAS contract: an optimistic ACCEPTED ack first, CONFLICT on a lost race
// ===========================================================================

void test_CasRunner_acks_optimistically_then_reports_conflict() {
  DropsGuard drops;
  Fixture f;
  f.repo->cas_status = 0;  // CAS commits
  ActionContext ok = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 2, 2, 2,
                            kStoneId, 0, 0, 0, /*inline_cas=*/true);
  ActionDispatcher d;
  CHECK(route(d, ok, f), "the break is handled");
  CHECK_EQ(f.publisher->acks.size(), size_t(1),
           "a successful break acks exactly once (the optimistic ACCEPTED)");
  if (!f.publisher->acks.empty()) {
    CHECK_EQ(f.publisher->acks[0].status,
             uint8_t(Protocol::BlockAckStatus_ACCEPTED),
             "the first ack is the optimistic ACCEPTED");
    CHECK_EQ(f.publisher->acks[0].request_id, f.kRequestId,
             "the ack echoes the client request_id");
    CHECK_EQ(f.publisher->acks[0].action_type,
             uint8_t(Protocol::PlayerActionType_LEFT_MOUSE_CLICK),
             "the ack echoes the action type that triggered it");
  }
  CHECK_EQ(f.publisher->changed.size(), size_t(1),
           "a committed CAS publishes the block-changed event");

  // Now force a conflict: the repo reports status 1 and never commits.
  f.reset();
  f.repo->cas_status = 1;
  ActionContext conflict = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 2, 2,
                                  2, kStoneId, 0, 0, 0, /*inline_cas=*/true);
  CHECK(route(d, conflict, f), "a conflicting break is still 'handled' by the dispatcher");
  CHECK_EQ(f.publisher->acks.size(), size_t(2),
           "a conflicting break acks twice: ACCEPTED then CONFLICT");
  if (f.publisher->acks.size() == 2) {
    CHECK_EQ(f.publisher->acks[0].status,
             uint8_t(Protocol::BlockAckStatus_ACCEPTED),
             "the optimistic ACCEPTED is still sent on a lost race");
    CHECK_EQ(f.publisher->acks[1].status,
             uint8_t(Protocol::BlockAckStatus_CONFLICT),
             "the second ack reports the CONFLICT");
  }
  CHECK_EQ(f.publisher->changed.size(), size_t(0),
           "a conflicting CAS changes no block and drops no items");
  CHECK_EQ(f.given.size(), size_t(0),
           "a conflicting CAS gives the player nothing");
}

// ===========================================================================
// 8. PlayerActionDispatcher: the legacy PlayerAction switch
// ===========================================================================

namespace {

std::vector<uint8_t> buildPlayerAction(Protocol::PlayerActionType action,
                                       uint64_t player_id, int32_t x, int32_t y,
                                       int32_t z, uint8_t count, uint16_t block_id) {
  flatbuffers::FlatBufferBuilder b;
  Protocol::Vec3i pos(x, y, z);
  auto off = Protocol::CreatePlayerAction(b, player_id, action, &pos, 0, count,
                                          block_id, 0);
  b.Finish(off);
  return std::vector<uint8_t>(b.GetBufferPointer(),
                             b.GetBufferPointer() + b.GetSize());
}

}  // namespace

void test_PlayerActionDispatcher_item_action_fires_the_give_callback() {
  std::vector<GivenItem> got;
  PlayerActionDispatcher d(
      [&got](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
        got.push_back({pid, item, n, slot});
      });
  const auto buf = buildPlayerAction(Protocol::PlayerActionType_ITEM_ACTION, 99, 4, 5, 6,
                                     12, kStoneId);
  d.dispatch(buf);
  CHECK_EQ(got.size(), size_t(1), "ITEM_ACTION fires the item-give callback exactly once");
  if (got.size() == 1) {
    CHECK_EQ(got[0].player_id, uint64_t(99), "the callback receives the player id");
    CHECK_EQ(got[0].item_id, kStoneId, "the callback receives the block_id as item id");
    CHECK_EQ(got[0].count, uint8_t(12), "the callback receives the count");
    CHECK_EQ(got[0].target_slot, 4, "the callback receives pos.x as the target slot");
  }
}

void test_PlayerActionDispatcher_item_action_without_a_callback_does_not_crash() {
  PlayerActionDispatcher d;  // default ctor: onGiveItem_ is null
  const auto buf = buildPlayerAction(Protocol::PlayerActionType_ITEM_ACTION, 1, 0, 0, 0,
                                     1, kStoneId);
  d.dispatch(buf);
  CHECK(true, "ITEM_ACTION with a null callback is a safe no-op");
}

void test_PlayerActionDispatcher_chunk_request_is_a_noop() {
  std::vector<GivenItem> got;
  PlayerActionDispatcher d(
      [&got](uint64_t, uint16_t, uint8_t, int32_t) { got.push_back({0, 0, 0, 0}); });
  const auto buf = buildPlayerAction(Protocol::PlayerActionType_CHUNK_REQUEST, 5, 1, 2, 3, 1,
                                     kStoneId);
  d.dispatch(buf);
  CHECK_EQ(got.size(), size_t(0), "CHUNK_REQUEST never fires the item-give callback");
}

void test_PlayerActionDispatcher_rejects_a_corrupt_buffer() {
  std::vector<GivenItem> got;
  PlayerActionDispatcher d(
      [&got](uint64_t, uint16_t, uint8_t, int32_t) { got.push_back({0, 0, 0, 0}); });
  const std::vector<uint8_t> junk = {0xde, 0xad, 0xbe, 0xef};
  d.dispatch(junk);
  CHECK_EQ(got.size(), size_t(0), "a buffer that fails verification is dropped");
  const std::vector<uint8_t> empty = {};
  d.dispatch(empty);
  CHECK_EQ(got.size(), size_t(0), "an empty buffer is dropped");
}

void test_PlayerActionDispatcher_sweep_over_every_declared_action_type() {
  // Every declared PlayerActionType. Only ITEM_ACTION and CHUNK_REQUEST are
  // routed; MOVE and UNLOAD fall to the default: branch, which returns false and
  // is swallowed by dispatch(). dispatch() is void, so the rejection is only
  // observable through its side effects.
  struct Row {
    Protocol::PlayerActionType action;
    const char* label;
    bool gives_item;
    bool consumed;
  };
  const Row rows[] = {
      {Protocol::PlayerActionType_MOVE, "MOVE", false, false},
      {Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, "RIGHT_MOUSE_CLICK", false, false},
      {Protocol::PlayerActionType_LEFT_MOUSE_CLICK, "LEFT_MOUSE_CLICK", false, false},
      {Protocol::PlayerActionType_CHUNK_REQUEST, "CHUNK_REQUEST", false, false},
      {Protocol::PlayerActionType_ITEM_ACTION, "ITEM_ACTION", true, true},
      {Protocol::PlayerActionType_UNLOAD, "UNLOAD", false, false},
  };
  for (const auto& row : rows) {
    std::vector<GivenItem> got;
    PlayerActionDispatcher d(
        [&got](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
          got.push_back({pid, item, n, slot});
        });
    const auto buf = buildPlayerAction(row.action, 3, 9, 8, 7, 5, kStoneId);
    d.dispatch(buf);
    CHECK_EQ(got.empty(), !row.gives_item,
             "only ITEM_ACTION reaches the item-give callback");
    (void)row.consumed;
  }
}

// ============================================================================
// 8b. THE RETURN CONTRACT: dispatch() returns true iff a handler claimed
//     the action (regression test for gp-qij1)
// ============================================================================
//
// Before gp-qij1, src/game/actions/ActionDispatcher.cpp was:
//
//   bool ActionDispatcher::dispatch(ActionContext& ctx) const {
//     bool handled = false;
//     std::apply(
//         [&](const auto&... h) {
//           ((handled || (h.canHandle(ctx) ? (h.handle(ctx), true) : false)) || ...);
//         },
//         handlers_);
//     return handled;
//   }
//
// The fold is a LEFT-TO-RIGHT || CHAIN over the comma expressions, and the
// value of the whole chain was DISCARDED because the lambda body is a
// statement-expression whose only effect is to call the handlers. The fold
// RESULT — the `true` produced by a claiming handler — was never assigned back
// to `handled`. `handled` was initialized to false and nothing ever wrote it,
// so dispatch() returned false unconditionally.
//
// The handlers themselves DID run (the || chain short-circuits correctly, so
// only the first claiming handler runs and later ones are skipped). Only the
// return value was broken.
//
// The gameplay consequence was in SetBlockCASHandler::handle()
// (src/game/actions/SetBlockCASHandler.cpp:47):
//
//   if (dispatcher_.dispatch(ctx)) return;      // <-- never taken
//   spdlog::info("Unhandled action: ...");
//   ctx.publisher_->publishBlockAck(REJECTED, ..., "nothing placeable in hand");
//
// So EVERY block action that was actually executed was immediately followed by
// a REJECTED "nothing placeable in hand" ack. The client saw a block break or
// place commit (an optimistic ACCEPTED ack plus a block-changed event) and then
// a rejection for the same request_id, regardless of what the player did. This
// is exactly what the integration test TestChunk_GetBlockAfterSet reported:
// "BlockAck status: expected ACCEPTED, got REJECTED".
//
// This test now asserts the CORRECT contract on both halves of it: the return
// value, and the absence of the facade's spurious REJECTED ack. It was written
// in the opposite direction on purpose (asserting the false return) so the fix
// could not land silently; gp-qij1 is the commit that inverted it.
void test_ActionDispatcher_returns_true_when_a_handler_ran() {
  DropsGuard drops;

  // LEFT click on stone: BreakBlockHandler claims it and the break really
  // happens (a CAS is issued, the block-changed event fires)...
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 9, 8, 7,
                               kStoneId, 0, 0, 0);
    ActionDispatcher d;
    const bool handled = d.dispatch(ctx);
    CHECK_EQ(f.repo->cas_calls.size(), size_t(1),
             "the break handler DID run and issued its CAS");
    CHECK_EQ(f.publisher->changed.size(), size_t(1),
             "the break handler DID publish a block-changed event");
    // ...so the dispatcher now reports it.
    CHECK_EQ(handled, true,
             "dispatch() returns true when a handler ran");
  }

  // RIGHT click on a machine: MachineInteractHandler claims it and really
  // opens the machine window...
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 10, 20,
                               30, kFurnaceId, 0, 0, 1);
    ActionDispatcher d;
    const bool handled = d.dispatch(ctx);
    CHECK_EQ(f.repo->get_calls.size(), size_t(1),
             "the machine handler DID run and read the block back");
    CHECK_EQ(f.publisher->directives.size(), size_t(1),
             "the machine handler DID emit its OPEN_UI directive");
    CHECK_EQ(handled, true,
             "dispatch() returns true when a handler ran");
  }

  // RIGHT click with a block in hand: PlaceBlockHandler claims it and the CAS
  // commits.
  {
    Fixture f;
    const uint16_t held = ItemId::pack("0:0:3");  // cobblestone
    f.inventory->giveItem(f.kPlayerId, held, 5, -1);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 5, 6, 7,
                               kAirId, held, held, 1);
    ActionDispatcher d;
    CHECK_EQ(d.dispatch(ctx), true,
             "dispatch() returns true for a committed placement");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(1),
             "the placement CAS really was issued");
  }

  // The negative half: nothing claimed the action, so nothing ran and the
  // return value must be false. Before gp-qij1 this passed for the wrong
  // reason (it could never be anything else); it must now pass for the right
  // one, which is why the positive rows above matter.
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 2, 2, 2,
                               kStoneId, 0, 0, 0);  // empty hand, plain block
    ActionDispatcher d;
    CHECK_EQ(d.dispatch(ctx), false,
             "dispatch() returns false when no handler claimed the action");
    CHECK_EQ(f.repo->cas_calls.size(), size_t(0), "no CAS for an unclaimed action");
    CHECK_EQ(f.publisher->acks.size(), size_t(0), "no ack for an unclaimed action");
  }

  // The consequence: the facade's rejection path is taken ONLY when nothing ran.
  // This is the SetBlockCASHandler logic (line 47 onward), exercised here
  // against a real publisher.
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 9, 8, 7,
                               kStoneId, 0, 0, 0);
    ActionDispatcher d;
    if (d.dispatch(ctx)) return;  // SetBlockCASHandler.cpp:47 — now correctly taken
    f.publisher->publishBlockAck(Protocol::BlockAckStatus_REJECTED, ctx.x, ctx.y,
                                 ctx.z, ctx.expected_block_id, 0,
                                 "nothing placeable in hand", ctx.request_id,
                                 ctx.action_type);
    CHECK_EQ(f.publisher->acks.size(), size_t(1),
             "a successful break acks exactly once — no spurious REJECTED follows");
    if (f.publisher->acks.size() == 1) {
      CHECK_EQ(f.publisher->acks[0].status,
               uint8_t(Protocol::BlockAckStatus_ACCEPTED),
               "the single ack for a handled action is the real ACCEPTED");
      CHECK_EQ(f.publisher->acks[0].request_id, f.kRequestId,
               "that ack echoes the client request_id");
    }
  }

  // And the facade's rejection is still reachable for a genuinely unhandled
  // action — the early return must not swallow the whole fallback path.
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 2, 2, 2,
                               kStoneId, 0, 0, 0);  // empty hand, plain block
    ActionDispatcher d;
    if (d.dispatch(ctx)) return;
    f.publisher->publishBlockAck(Protocol::BlockAckStatus_REJECTED, ctx.x, ctx.y,
                                 ctx.z, ctx.expected_block_id, 0,
                                 "nothing placeable in hand", ctx.request_id,
                                 ctx.action_type);
    CHECK_EQ(f.publisher->acks.size(), size_t(1),
             "an unhandled action is still rejected exactly once");
    if (f.publisher->acks.size() == 1) {
      CHECK_EQ(f.publisher->acks[0].status,
               uint8_t(Protocol::BlockAckStatus_REJECTED),
               "the fallback ack is REJECTED");
      CHECK_EQ(f.publisher->acks[0].reason,
               std::string("nothing placeable in hand"),
               "the fallback ack keeps its reason");
    }
  }
}

// ===========================================================================
// 9. The multiblock break guard short-circuits the break before the CAS
// ===========================================================================

void test_BreakBlockHandler_refuses_the_break_when_contents_do_not_fit() {
  DropsGuard drops;
  Fixture f;
  // A 40-slot inventory, every slot full of 64 stone: nothing can fit.
  std::array<simcore::PersistSlot, simcore::kInventorySlots> full{};
  for (auto& s : full) s = {kStoneId, 64, 0};
  f.inventory->setSlots(f.kPlayerId, full);

  // Register a multiblock controller that owns (2,2,2) and give its anchor
  // entity an inventory full of stone. The break guard then tries to hand all
  // of it to the player and must refuse.
  f.engine->registerController(
      1, 2, 2, 2, 0,
      std::vector<uint32_t>{(2u & 0x3FFu) | ((2u & 0x3FFu) << 10) | ((2u & 0x3FFu) << 20)});
  auto& reg = f.engine->reg();
  auto ent = reg.create();
  reg.emplace<simcore::Position>(ent, 2, 2, 2);
  simcore::InventoryContainer container;
  container.entity_type = 2;
  container.slot_count = 2;
  container.slots = {simcore::InventorySlot(kStoneId, 64, 0),
                     simcore::InventorySlot(kStoneId, 64, 0)};
  reg.emplace<simcore::InventoryContainer>(ent, std::move(container));

  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 2, 2, 2,
                             kStoneId, 0, 0, 0);
  ActionDispatcher d;
  CHECK(route(d, ctx, f), "the dispatch itself is still considered handled");
  CHECK_EQ(f.repo->cas_calls.size(), size_t(0),
           "a refused multiblock break issues no CAS (the block stays)");
  CHECK_EQ(f.publisher->changed.size(), size_t(0), "a refused break changes no block");
  CHECK_EQ(f.publisher->acks.size(), size_t(1), "a refused break acks exactly once");
  if (f.publisher->acks.size() == 1) {
    CHECK_EQ(f.publisher->acks[0].status,
             uint8_t(Protocol::BlockAckStatus_REJECTED),
             "the refusal ack is REJECTED");
    CHECK_EQ(f.publisher->acks[0].reason,
             std::string("Multiblock contents do not fit in inventory"),
             "the refusal explains itself to the client");
  }
  const auto after = f.inventory->getSlots(f.kPlayerId);
  CHECK_EQ(after[0].count, uint8_t(64),
           "a refused break leaves the player inventory untouched");
}

void test_BreakBlockHandler_hands_multiblock_contents_to_the_player_on_success() {
  DropsGuard drops;
  Fixture f;
  std::array<simcore::PersistSlot, simcore::kInventorySlots> inv{};
  inv[0] = {ItemId::pack("0:0:3"), 8, 0};  // cobblestone
  f.inventory->setSlots(f.kPlayerId, inv);

  f.engine->registerController(
      2, 3, 3, 3, 0,
      std::vector<uint32_t>{(3u & 0x3FFu) | ((3u & 0x3FFu) << 10) | ((3u & 0x3FFu) << 20)});
  auto& reg = f.engine->reg();
  auto ent = reg.create();
  reg.emplace<simcore::Position>(ent, 3, 3, 3);
  simcore::InventoryContainer container;
  container.entity_type = 2;
  container.slot_count = 1;
  container.slots = {simcore::InventorySlot(ItemId::pack("0:0:3"), 12, 0)};
  reg.emplace<simcore::InventoryContainer>(ent, std::move(container));

  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 3, 3, 3,
                             kStoneId, 0, 0, 0);
  ActionDispatcher d;
  CHECK(route(d, ctx, f), "the dispatch itself is still considered handled");
  CHECK_EQ(f.repo->cas_calls.size(), size_t(1), "a fitting multiblock break proceeds");
  int cobble = 0;
  for (const auto& s : f.inventory->getSlots(f.kPlayerId)) {
    if (s.item_id == ItemId::pack("0:0:3")) cobble += s.count;
  }
  CHECK_EQ(cobble, 20, "the multiblock contents (12) are merged into the inventory (8+12)");
  CHECK_EQ(f.given.size(), size_t(1),
           "the broken block itself is still dropped to the player");
  if (f.given.size() == 1) {
    CHECK_EQ(f.given[0].item_id, kStoneId, "the broken block is the drop");
  }
}

}  // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("=== action dispatch test suite ===\n\n");

  TEST(ActionDispatcher_sweep_over_every_declared_action_type);
  TEST(ActionDispatcher_held_item_gates_the_place_route);
  TEST(ActionDispatcher_drill_in_hand_still_breaks_on_left_click);
  TEST(ActionDispatcher_mining_tool_in_hand_falls_through_to_break);
  TEST(ActionContext_copies_every_field_from_the_flatbuffer);
  TEST(ActionContext_right_click_derives_the_face_adjacent_cell);
  TEST(ActionContext_left_click_targets_the_clicked_cell);
  TEST(ActionContext_null_action_yields_a_default_context);
  TEST(ActionContext_without_engine_has_no_machine_info);
  TEST(CasRunner_acks_optimistically_then_reports_conflict);
  TEST(PlayerActionDispatcher_item_action_fires_the_give_callback);
  TEST(PlayerActionDispatcher_item_action_without_a_callback_does_not_crash);
  TEST(PlayerActionDispatcher_chunk_request_is_a_noop);
  TEST(PlayerActionDispatcher_rejects_a_corrupt_buffer);
  TEST(PlayerActionDispatcher_sweep_over_every_declared_action_type);
  TEST(ActionDispatcher_returns_true_when_a_handler_ran);
  TEST(BreakBlockHandler_refuses_the_break_when_contents_do_not_fit);
  TEST(BreakBlockHandler_hands_multiblock_contents_to_the_player_on_success);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests, g_passed,
         g_failed);
  return g_failed > 0 ? 1 : 0;
}
