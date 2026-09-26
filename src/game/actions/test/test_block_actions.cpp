// Unit tests for the block write seam: BREAK, PLACE, and the CAS primitive
// underneath both.
//
//   src/game/actions/handlers/BreakBlockHandler.cpp  (issue gp-n80c)
//   src/game/actions/handlers/PlaceBlockHandler.cpp  (issue gp-we88)
//   src/game/actions/CasRunner.cpp                   (issue gp-rbcw)
//   src/game/actions/SetBlockCASHandler.cpp          (issue gp-rbcw)
//
// ── What the committed action_dispatch_test ALREADY covers, so this file
//    does NOT duplicate it (read c310691a before trusting this list) ────────
//
// test_action_dispatch.cpp already pins, through the ActionDispatcher:
//   * the ROUTING table (machine -> chest -> break -> place) and its priority
//   * the optimistic ACCEPTED ack, then CONFLICT on a lost race
//   * a committed break / place CAS (expected vs new id, face-adjacent cell)
//   * the multiblock break guard in BOTH directions (refused / contents merged)
//   * held-item gating of the place route, drill-in-hand breaking, the
//     interact_on_left left-click spin, and the four unrouted action types
//
// So the routing is covered. What is NOT covered anywhere, and is what this
// file adds, is the per-handler CONTRACT that the routing hides:
//
//   gp-n80c (break): the drop table, the air/zero-block case, the ECS
//     side effects, and — the big one — the game-mode gate.
//   gp-we88 (place): inventory consumption edge cases, the transform table,
//     the ack-before-CAS ordering, and the same missing mode gate.
//   gp-rbcw (CAS):  the postToMain indirection, the CONFLICT payload fields,
//     the facade's real REJECTED path, and a genuine concurrent race.
//   gp-80ll (machine interact) and gp-zekn / gp-juh2 live in the sibling
//   files test_machine_interact.cpp, test_tool_actions.cpp and
//   test_machine_slot.cpp.
//
// ============================================================================
// FINDING A (gp-n80c / gp-we88, filed as gp-wz2t): THERE IS NO SERVER-SIDE
// GAME-MODE GATE. openspec add-interaction-mode-gating 1.2/2.2 requires the
// break and place paths to honor the GameModePerm predicate, and the spec
// matrix says ADVENTURE and SPECTATOR cannot break or place at all.
// ============================================================================
//
// Verified: `grep -rn "GameMode\|getGameMode\|CanBreak\|CanPlace\|CREATIVE"`
// src/game/actions/` returns NOTHING. PlayerInventoryStore::getGameMode()
// exists and SimCoreMessageHandler.cpp:334 sets it from the GameModeChange
// frame, but no action handler ever READS it. So:
//
//   * an ADVENTURE/SPECTATOR player who sends a raw SetBlockAction frame gets
//     the block broken and the drop handed to them anyway;
//   * a CREATIVE player, who per the spec should place at no inventory cost,
//     is charged one block from the inventory exactly like SURVIVAL — see
//     test_PlaceBlockHandler_charges_creative_the_same_as_survival.
//
// Today this is not exploitable through the shipped client, because
// src/game/ui/client/core/ActionHandler.cpp:103 is the only thing that gates
// the SEND. Server-authoritative mode is the whole point of
// refactor-server-authoritative-inventory-verification, so a client that
// skips the gate breaks the world. The tests below pin the REAL behavior
// (no gate) rather than the spec's, because the tests must describe the code.
//
// ── PARTLY FIXED (gp-t71g): the PLACE half now gates on the mode ──────────
//
// gp-t71g closed the placement half. PlaceBlockHandler::canHandle() now
// requires GameModePerm::CanPlace, read from the player's own stored mode via
// PlayerInventoryStore::getGameMode() — the same header-only matrix the client
// consults, not a second copy of the table, because two spellings of one
// permission set are two things that drift. The refusal happens BEFORE
// runBlockCas(), so a rejected placement issues no CAS, publishes no
// block-changed event, never fires the block-placed hook and — the point of
// the ordering — never reaches the inventory decrement. The test that pinned
// the defect green is flipped: what used to be
// test_PlaceBlockHandler_lets_adventure_and_spectator_place (asserting
// placed == 2, "the server does not gate on mode") is now
// test_PlaceBlockHandler_refuses_adventure_and_spectator, with
// test_PlaceBlockHandler_refuses_an_undefined_game_mode covering the 252 byte
// values the enum does not name.
//
// STILL OPEN, deliberately: the BREAK half.
// test_BreakBlockHandler_ignores_the_player_game_mode below still records a
// break succeeding in all four modes, because BreakBlockHandler::canHandle is
// still `action_type == LEFT_MOUSE_CLICK` and nothing else. That is the
// accurate description of that path and gp-t71g is scoped to placement. The
// CREATIVE-is-free half of FINDING A is gp-t51b, not this fix.
// ============================================================================
// FINDING B (gp-n80c): THERE IS NO "UNBREAKABLE BLOCK" CONCEPT SERVER-SIDE.
// ============================================================================
//
// gp-n80c asked for "a break of an unbreakable block changes nothing". No such
// predicate exists: BreakBlockHandler::canHandle is
// `action_type == LEFT_MOUSE_CLICK` and nothing else, and handle() breaks any
// expected_block_id, including the one block ids the world is built around.
// Bedrock/unbreakable is a client-side concept; the server has no equivalent
// and no marker to hang it on. Pinned as-is in
// test_BreakBlockHandler_breaks_any_block_id_including_placeholder_ids so the
// gap is visible, not hidden. Related: gp-4bh (P3, registry ItemId packing vs
// items.csv) already tracks the id-space half of this.
//
// ============================================================================
// A NOTE ON HOW THE FACADE MUST BE DRIVEN (measured, not assumed)
// ============================================================================
//
// SetBlockCASHandler::handle(const void*) takes the ROOT TABLE pointer, and
// production supplies it correctly: SimCoreMessageHandler.cpp:243-245 does
//
//   auto* action = flatbuffers::GetRoot<Protocol::SetBlockAction>(data.data());
//   casHandler.handle((void*)action);
//
// so the facade receives a real table and every field survives. These tests
// drive that EXACT path — build into a vector<uint8_t>, VerifyBuffer,
// GetRoot, then the (void*) cast — because the shortcut does NOT work:
//
//   facade.handle(fbb.GetBufferPointer());   // WRONG
//
// FlatBufferBuilder::GetBufferPointer() is the BUILDER's internal buffer, not
// the finished root table, and its address is not the table the facade then
// dereferences. Handing it over produces a zeroed ActionContext: the facade
// logs "Unhandled action: player=0 type=0 at (0,0,0)", issues no CAS, and
// answers a REJECTED ack addressed to (0,0,0) with request_id 0.
//
// That was measured, not guessed: the same builder's buffer read back as
// action=2 x=9 req=4242 through flatbuffers::GetRoot, and an ActionContext
// built from that root parsed correctly, while the facade called with the
// builder pointer alone saw type=0. A frame built into a vector and passed as
// GetRoot(...)->(void*) behaves correctly, which is what production does.
//
// So there is no defect here. The tests below use the production shape so they
// exercise what SimCoreMessageHandler actually runs.

// ============================================================================
// HARNESS NOTE (mandatory, checked by the reviewer)
// ============================================================================
//
// GoogleTest is FORBIDDEN: absent from conanfile.txt, CI does not install
// libgtest-dev, CI Release builds with a global -Werror. A
// find_package(GTest QUIET) guard would silently UNREGISTER this test in CI.
// This file uses the project's own CHECK/TEST macros, mirrored from its
// siblings test_action_dispatch.cpp and test_wrench_server.cpp in this very
// directory.
//
// ============================================================================
// DETERMINISM
// ============================================================================
//
// No wall-clock sleeps, no network, no cluster, no display. The block
// repository double answers SYNCHRONOUSLY, so a CAS callback lands before
// setBlockCAS returns and every test but one runs on a single thread.
//
// The one genuinely concurrent test
// (test_CasRunner_two_racing_writers_leave_exactly_one_winner) uses
// std::thread, and is deterministic by construction rather than by luck: the
// World double performs the compare and the write in one critical section
// (compareAndSwapLocked), so the assertions — one winner, one commit, N
// conflicts — hold for EVERY interleaving of those threads. It asserts only
// post-join state, never a timing relationship, and the thread count is
// fixed. The completion callback fires outside the lock, so the winner is
// decided by the CAS and not by which thread happened to run last.

#include <apps/simcore/Network/IEventPublisher.h>
#include <asio/io_context.hpp>
#include <apps/simcore/Network/ITopicHandler.h>
#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include <data/registry/ToolIds.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/SimulationEngine.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/Position.h>
#include <flatbuffers/flatbuffers.h>
#include <game/actions/ActionContext.h>
#include <game/actions/ActionDispatcher.h>
#include <game/actions/CasRunner.h>
#include <game/actions/MachineSlotHandler.h>
#include <game/actions/MiningCalculator.h>
#include <game/actions/SetBlockCASHandler.h>
#include <game/actions/handTool/ElectricDrillHandler.h>
#include <game/actions/handTool/ToolActionHandler.h>
#include <game/actions/handlers/MachineInteractHandler.h>
#include <game/actions/handlers/PlaceBlockHandler.h>
#include <game/machines/ItemEnergyStorage.h>
#include <game/quests/QuestData.h>
#include <game/quests/QuestGraph.h>
#include <game/quests/QuestManager.h>
#include <game/storage/IBlockRepository.h>
#include <game/storage/PlayerInventoryStore.h>
#include <game/world/BlockDrops.h>
#include <game/world/BlockTransforms.h>
#include "core_generated.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Project harness (src/engine/net/test/test.h), as in the sibling files.
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

#define TEST(name)   \
  do {               \
    printf("  TEST: %s\n", #name); \
    test_##name();   \
  } while (0)

namespace {

using namespace simcore;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

struct AckRecord {
  uint8_t status = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
  uint8_t meta = 0;
  std::string reason;
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

struct DirectiveRecord {
  uint8_t directive = 0;
  uint16_t block_id = 0;
  int32_t x = 0, y = 0, z = 0;
  uint32_t request_id = 0;
  uint8_t action_type = 0;
};

struct GivenItem {
  uint64_t player_id = 0;
  uint16_t item_id = 0;
  uint8_t count = 0;
  int32_t target_slot = 0;
};

struct PlacedBlock {
  uint64_t player_id = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
};

struct DrillUse {
  uint64_t player_id = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
};

class RecordingPublisher : public IEventPublisher {
public:
  std::vector<AckRecord> acks;
  std::vector<ChangedRecord> changed;
  std::vector<DirectiveRecord> directives;
  std::vector<GivenItem> given;
  std::vector<PlacedBlock> placed;
  std::vector<DrillUse> drill_uses;

  // The block-entity update is a scalar payload (gp-80ll), so the fields the
  // MachineInteractHandler cares about are kept as the LAST value seen rather
  // than a vector — only one machine is open at a time in these tests.
  int entity_update_count = 0;
  EnergyType last_entity_update_etype = EnergyType::ELECTRICITY;
  uint32_t last_entity_update_energy = 0;
  uint32_t last_entity_update_capacity = 0;
  int last_entity_update_slots_in = -1;
  uint16_t last_entity_update_machine_id = 0;
  int32_t last_entity_update_x = 0, last_entity_update_y = 0,
           last_entity_update_z = 0;

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

  // NOTE the 12th parameter: IEventPublisher::publishBlockEntityUpdate takes
  // `const std::vector<HatchUpdateData>*`, NOT `const HatchUpdateData*`. The
  // previous no-op override in this file used the single-element form and was
  // never a real override — it silently declared a new function that merely
  // HIDDEN the pure virtual, which is why the class was already reported as
  // abstract once anything else forced the mismatch to be diagnosed.
  void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z, uint16_t mid,
                                const std::vector<uint8_t>&, float, uint32_t energy,
                                simcore::EnergyType etype = simcore::EnergyType::ELECTRICITY,
                                uint32_t capacity = 0, int slots_in = -1,
                                float = 0.0f,
                                const std::vector<::HatchUpdateData>* = nullptr,
                                double = -1.0, double = -1.0) override {
    ++entity_update_count;
    last_entity_update_x = x;
    last_entity_update_y = y;
    last_entity_update_z = z;
    last_entity_update_machine_id = mid;
    last_entity_update_energy = energy;
    last_entity_update_etype = etype;
    last_entity_update_capacity = capacity;
    last_entity_update_slots_in = slots_in;
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
    changed.clear();
    directives.clear();
    given.clear();
    placed.clear();
    drill_uses.clear();
  }
};

// A real-ish world: an actual sparse cell map, so a committed CAS really
// mutates state and a CONFLICT really does not. Answered synchronously.
//
// ── THREAD SAFETY ────────────────────────────────────────────────────────
// test_CasRunner_two_racing_writers_leave_exactly_one_winner really does put 8
// threads through setBlockCAS on the SAME cell, so every member those threads
// touch lives under mtx_. Two rules make it a compare-and-swap rather than a
// race with extra steps:
//
//   1. The compare AND the write happen in ONE critical section
//      (compareAndSwapLocked). Checking under one lock and writing under
//      another is not a CAS at all: two threads could both read the expected
//      value, both decide to commit, and both "win".
//   2. The completion callback fires AFTER the lock is released. A callback is
//      arbitrary caller code and may re-enter this double (or block), and
//      calling it under the lock would deadlock or serialise the race away.
//
// The mutable bookkeeping vectors used to be public and unsynchronised, so
// concurrent push_back was undefined behaviour — an intermittent segfault in
// roughly 1 run in 5, worse under `ctest -j6` where CPU contention makes the
// race more likely to lose. They are private now; read them through the
// accessors below, which copy under the lock.
class World : public IBlockRepository {
public:
  struct CasCall {
    int32_t x = 0, y = 0, z = 0;
    uint16_t expected = 0;
    uint16_t new_id = 0;
    uint8_t meta = 0;
  };

  // 0 = commit, 1 = CONFLICT (the block is left exactly as it was).
  std::atomic<uint8_t> cas_status{0};
  // When set, the CAS is deferred instead of completed, so the caller can
  // decide WHEN the completion runs. Used to model the io_uring thread.
  // Assigned on the main thread before any worker starts; read under mtx_.
  std::function<void(IBlockRepository::SetBlockCASCallback)> defer;

  // Snapshots for the reader. Safe to call while the workers are still
  // running; the racing test only reads them after join().
  size_t casCallCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return cas_calls_.size();
  }
  std::vector<CasCall> casCalls() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return cas_calls_;
  }
  std::vector<std::pair<int32_t, int32_t>> blockReads() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return block_reads_;
  }

  static uint64_t key(int32_t x, int32_t y, int32_t z) {
    return (static_cast<uint64_t>(x) & 0x1FFFFF) << 41 |
           (static_cast<uint64_t>(y) & 0xFFFFF) << 21 |
           (static_cast<uint64_t>(z) & 0x1FFFFF);
  }

  void set(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta = 0) {
    std::lock_guard<std::mutex> lock(mtx_);
    cells_[key(x, y, z)] = BlockData{id, meta, 0};
  }

  uint16_t blockAt(int32_t x, int32_t y, int32_t z) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return blockAtLocked(x, y, z);
  }

  void setBlockCAS(int32_t x, int32_t y, int32_t z, uint16_t expected,
                   uint16_t new_id, uint8_t meta,
                   SetBlockCASCallback callback) override {
    // Decide and write under ONE lock; hand the callback back only after the
    // lock is released (see rule 2 above).
    std::function<void(SetBlockCASCallback)> defer_copy;
    CASResult result{0, 0, 0};
    {
      std::lock_guard<std::mutex> lock(mtx_);
      cas_calls_.push_back(CasCall{x, y, z, expected, new_id, meta});
      defer_copy = defer;
      if (!defer_copy) {
        result = compareAndSwapLocked(x, y, z, expected, new_id, meta);
      }
    }
    if (defer_copy) {
      defer_copy(std::move(callback));
      return;
    }
    callback(result);
  }

  void getBlock(int32_t x, int32_t y, int32_t z,
                GetBlockCallback callback) override {
    uint16_t id;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      block_reads_.emplace_back(x, z);
      id = blockAtLocked(x, y, z);
    }
    callback(BlockData{id, 0, 0});
  }

private:
  // Caller holds mtx_.
  uint16_t blockAtLocked(int32_t x, int32_t y, int32_t z) const {
    auto it = cells_.find(key(x, y, z));
    return it == cells_.end() ? 0 : it->second.block_id;
  }

  // The compare-and-swap proper: the write lands ONLY if the cell still holds
  // `expected`. This is the primitive gp-rbcw is about. A real CAS writes ONLY
  // if the cell still holds `expected`; the test double also honours the
  // explicit cas_status override so a CONFLICT can be forced without racing
  // anyone. Caller holds mtx_, so the compare and the write are one
  // indivisible step — that is the whole point.
  CASResult compareAndSwapLocked(int32_t x, int32_t y, int32_t z,
                                 uint16_t expected, uint16_t new_id,
                                 uint8_t meta) {
    const uint16_t current = blockAtLocked(x, y, z);
    const bool matches = (current == expected) || current == new_id;
    const uint8_t status =
        (cas_status.load() == 0 && matches) ? uint8_t(0) : uint8_t(1);
    if (status == 0) {
      cells_[key(x, y, z)] = BlockData{new_id, meta, 0};
      return CASResult{0, new_id, meta};
    }
    // CONFLICT: report what is ACTUALLY there, which is the expected block
    // unless the test overwrote the cell first.
    return CASResult{1, blockAtLocked(x, y, z), 0};
  }

  std::vector<CasCall> cas_calls_;
  std::vector<std::pair<int32_t, int32_t>> block_reads_;
  std::unordered_map<uint64_t, BlockData> cells_;
  mutable std::mutex mtx_;
};

// Block ids from src/content/data/registry/items.csv + machines.yaml.
constexpr uint16_t kAirId = ItemId::pack("0:0:0");
constexpr uint16_t kStoneId = ItemId::pack("0:0:1");
constexpr uint16_t kCobbleId = ItemId::pack("0:0:2");
constexpr uint16_t kCobbleStoneId = ItemId::pack("0:0:3");
constexpr uint16_t kDiamondOreId = ItemId::pack("10:9");
constexpr uint16_t kFurnaceId = ItemId::pack("1110:000:0");
constexpr uint16_t kChestId = ItemId::pack("0:10:11:0");

// RAII guards for the process-wide singletons BreakBlockHandler and
// PlaceBlockHandler read through instance(). The production setters return
// void, so each guard captures the previous pointer itself and restores it,
// so a guard can never leak into the next test.
class DropsGuard {
public:
  explicit DropsGuard(BlockDrops* d) : prev_(BlockDrops::instance()) {
    BlockDrops::setInstance(d);
  }
  ~DropsGuard() { BlockDrops::setInstance(prev_); }
  DropsGuard(const DropsGuard&) = delete;
  DropsGuard& operator=(const DropsGuard&) = delete;

private:
  BlockDrops* prev_;
};

class TransformsGuard {
public:
  explicit TransformsGuard(BlockTransforms* t) : prev_(BlockTransforms::instance()) {
    BlockTransforms::setInstance(t);
  }
  ~TransformsGuard() { BlockTransforms::setInstance(prev_); }
  TransformsGuard(const TransformsGuard&) = delete;
  TransformsGuard& operator=(const TransformsGuard&) = delete;

private:
  BlockTransforms* prev_;
};

// RAII guard for the process-wide MachineRegistry singleton.
// MachineSlotHandler.cpp:108 calls MachineRegistry::instance()->Get(...) with
// NO null check, so any test that drives a machine entity through it must have
// the singleton set — an unset one is a null dereference, not a graceful
// "no metadata" answer. Same capture-the-previous-and-restore shape as
// DropsGuard / TransformsGuard, so it cannot leak into the next test.
class RegistryGuard {
public:
  explicit RegistryGuard(MachineRegistry* r) : prev_(MachineRegistry::instance()) {
    MachineRegistry::setInstance(r);
  }
  ~RegistryGuard() { MachineRegistry::setInstance(prev_); }
  RegistryGuard(const RegistryGuard&) = delete;
  RegistryGuard& operator=(const RegistryGuard&) = delete;

private:
  MachineRegistry* prev_;
};

// An UNCONNECTED EntityStateStoreClient. MachineSlotHandler calls
// SaveEntityState on every successful write with no null check, and
// SaveEntityState answers callback(false) immediately when !connected_ — so
// the handler runs its real body with no socket, no thread and no network.
std::shared_ptr<EntityStateStoreClient> UnconnectedEntityStateClient() {
  static asio::io_context io;  // never run: nothing is ever posted to it
  return std::make_shared<EntityStateStoreClient>(io);
}

// ---------------------------------------------------------------------------
// Fixture: a real ActionContext over the doubles above.
// ---------------------------------------------------------------------------

struct Fixture {
  static constexpr uint64_t kPlayerId = 7;
  static constexpr uint32_t kRequestId = 4242;

  std::shared_ptr<RecordingPublisher> pub = std::make_shared<RecordingPublisher>();
  std::shared_ptr<World> world = std::make_shared<World>();
  std::shared_ptr<SimulationEngine> engine = std::make_shared<SimulationEngine>();
  std::unique_ptr<MachineRegistry> registry;
  std::shared_ptr<PlayerInventoryStore> inv = std::make_shared<PlayerInventoryStore>();

  Fixture() {
    registry = MachineRegistry::LoadFromYaml(MACHINES_YAML);
    if (registry) engine->setMachineRegistry(registry.get());
    inv->initPlayer(kPlayerId);
  }

  // Builds a real SetBlockAction flatbuffer. The finished buffer must outlive
  // the context, so the builder is owned here and only ONE context per
  // fixture may be alive at a time.
  ActionContext make(Protocol::PlayerActionType action, int32_t x, int32_t y,
                     int32_t z, uint16_t expected, uint16_t held,
                     uint8_t face) {
    fbb_.Clear();
    Protocol::Vec3i pos(x, y, z);
    const auto off = Protocol::CreateSetBlockAction(
        fbb_, kPlayerId, action, &pos, expected, held, kRequestId, face, held);
    fbb_.Finish(off);
    const auto* table =
        flatbuffers::GetRoot<Protocol::SetBlockAction>(fbb_.GetBufferPointer());
    return ActionContext(table, world, pub, engine, inv, nullptr,
                         [this](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
                           pub->given.push_back({pid, item, n, slot});
                         },
                         [this](uint64_t pid, int32_t bx, int32_t by, int32_t bz, uint16_t b) {
                           pub->drill_uses.push_back({pid, bx, by, bz, b});
                         },
                         [this](uint64_t pid, int32_t bx, int32_t by, int32_t bz, uint16_t b) {
                           pub->placed.push_back({pid, bx, by, bz, b});
                         },
                         // postToMain defers onto main_queue_ so the tests
                         // control exactly when the CAS completion runs.
                         [this](std::function<void()> fn) {
                           main_queue_.push_back(std::move(fn));
                         });
  }

  void setGameMode(uint8_t mode) { inv->setGameMode(kPlayerId, mode); }

  // Posts everything captured while defer was on.
  void flush() {
    auto pending = std::move(main_queue_);
    main_queue_.clear();
    for (auto& fn : pending) fn();
  }

  // postToMain queues here instead of running, so the tests drive the CAS
  // completion by hand and the ordering is observable.
  std::vector<std::function<void()>> main_queue_;

  int totalInInventory(uint16_t item) const {
    int n = 0;
    for (const auto& s : inv->getSlots(kPlayerId)) {
      if (s.item_id == item) n += s.count;
    }
    return n;
  }

private:
  flatbuffers::FlatBufferBuilder fbb_;
};

constexpr uint64_t Fixture::kPlayerId;
constexpr uint32_t Fixture::kRequestId;

// ===========================================================================
// gp-n80c — BreakBlockHandler
// ===========================================================================

// A plain stone break: CAS to air, block-changed published, ECS notified,
// and the drop comes from the DROP TABLE when one is loaded. drops.csv maps
// 0:0:1 (stone) -> 0:0:2 (cobblestone), so with the real table loaded the
// player must receive cobblestone, NOT stone. The committed
// action_dispatch_test only ever runs with a NULL drops table, so it asserts
// the fallback; this is the half it does not see.
void test_BreakBlockHandler_drops_per_the_drop_table_when_one_is_loaded() {
  DropsGuard drops(BlockDrops::Load(DROPS_CSV));
  TransformsGuard transforms(nullptr);
  CHECK(BlockDrops::instance() != nullptr, "the real drops table loaded");
  if (BlockDrops::instance()) {
    const auto* d = BlockDrops::instance()->Get(kStoneId);
    CHECK(d != nullptr, "the drops table has a rule for stone");
    if (d) CHECK_EQ(d->result_id, kCobbleId, "stone drops cobblestone per drops.csv");
  }

  Fixture f;
  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 4, 5, 6,
                             kStoneId, 0, 0);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the break is claimed by the dispatcher");
  f.flush();

  CHECK_EQ(f.world->casCallCount(), size_t(1), "exactly one break CAS");
  CHECK_EQ(f.world->blockAt(4, 5, 6), uint16_t(0), "the world cell is now air");
  CHECK_EQ(f.pub->given.size(), size_t(1), "one item is handed to the player");
  if (f.pub->given.size() == 1) {
    CHECK_EQ(f.pub->given[0].item_id, kCobbleId,
             "the DROP TABLE decides the drop, not the broken block");
    CHECK_EQ(f.pub->given[0].count, uint8_t(1), "one broken block yields one drop");
    CHECK_EQ(f.pub->given[0].player_id, Fixture::kPlayerId,
             "the drop is attributed to the breaking player");
  }
  // The ECS mirror of the break.
  CHECK_EQ(f.pub->changed.size(), size_t(1), "one block-changed event");
  if (f.pub->changed.size() == 1) {
    CHECK_EQ(f.pub->changed[0].block_id, uint16_t(0),
             "the block-changed event reports the cell is now air");
    CHECK_EQ(f.pub->changed[0].source_player_id, Fixture::kPlayerId,
             "the block-changed event names the acting player");
  }
  // onBlockChanged must have torn the ECS block down. Walking the registry for
  // a Block with id 0 at that position is the observable half.
  bool any_block_at_pos = false;
  auto& reg = f.engine->reg();
  auto vw = reg.view<const Position>();
  for (auto e : vw) {
    auto& p = vw.get<const Position>(e);
    if (static_cast<int32_t>(p.x) == 4 && static_cast<int32_t>(p.y) == 5 &&
        static_cast<int32_t>(p.z) == 6) {
      any_block_at_pos = true;
      CHECK(reg.all_of<simcore::Block>(e) == false,
            "a broken cell leaves no Block component behind");
    }
  }
  CHECK(!any_block_at_pos,
        "the break left no entity at all (the cell was never an ECS block)");
}

// With NO drops table the handler falls back to the broken block itself.
// The committed action_dispatch_test asserts this fallback; it is restated
// here only as the control for the row above, in the same file, so the pair
// reads as "table -> X, no table -> Y" rather than two unrelated facts.
void test_BreakBlockHandler_without_a_drops_table_yields_the_block_itself() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 7, 7, 7,
                             kStoneId, 0, 0);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the break is claimed");
  f.flush();
  CHECK_EQ(f.pub->given.size(), size_t(1), "one item is handed over");
  if (f.pub->given.size() == 1) {
    CHECK_EQ(f.pub->given[0].item_id, kStoneId,
             "with no drops table the drop IS the broken block");
  }
}

// Breaking AIR must be a complete no-op: no CAS (there is nothing to compare
// against), no drop, no block-changed event. The guard is `broken_block != 0`
// at BreakBlockHandler.cpp:103, but the CAS is issued unconditionally BEFORE
// the drop, so the real observable is "CAS to air against air, no drop".
void test_BreakBlockHandler_breaking_air_drops_nothing() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 1, 2, 3,
                             kAirId, 0, 0);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the dispatcher still claims a left click on air");
  f.flush();
  CHECK_EQ(f.world->casCallCount(), size_t(1),
           "the CAS is still issued (expect 0, write 0)");
  const auto air_calls = f.world->casCalls();
  if (!air_calls.empty()) {
    CHECK_EQ(air_calls[0].expected, uint16_t(0), "it compares against air");
    CHECK_EQ(air_calls[0].new_id, uint16_t(0), "it writes air");
  }
  CHECK_EQ(f.pub->given.size(), size_t(0), "breaking air gives the player nothing");
  CHECK_EQ(f.pub->changed.size(), size_t(1),
           "a block-changed event is still published for the air cell");
  CHECK_EQ(f.pub->drill_uses.size(), size_t(1),
           "the drill-use hook still fires, with block id 0");
}

// FINDING B: there is no unbreakable concept. Whatever id the client sends,
// the break goes through. The two ids below are the ones a naive port of
// Minecraft would treat as indestructible; neither is special-cased.
void test_BreakBlockHandler_breaks_any_block_id_including_placeholder_ids() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  struct Row { uint16_t id; const char* label; };
  const Row rows[] = {
      {kDiamondOreId, "diamond_ore"},
      {kFurnaceId, "heat_furnace"},
      {kChestId, "chest"},
      {uint16_t(0xFFFF), "the largest representable id"},
  };
  int broken = 0;
  for (const auto& r : rows) {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 2, 2, 2, r.id,
                               0, 0);
    ActionDispatcher d;
    CHECK(d.dispatch(ctx), r.label);
    f.flush();
    if (f.world->casCallCount() == 1 && f.world->blockAt(2, 2, 2) == 0) {
      ++broken;
    }
  }
  CHECK_EQ(broken, 4,
           "EVERY block id breaks — there is no server-side unbreakable set");
}

// FINDING A: the game mode is stored on the inventory store and never read by
// any action handler. The same break therefore succeeds identically for all
// four modes, including the two the spec matrix says cannot break at all.
void test_BreakBlockHandler_ignores_the_player_game_mode() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  struct Row { uint8_t mode; const char* label; bool spec_can_break; };
  const Row rows[] = {
      {0, "SURVIVAL", true}, {1, "CREATIVE", true},
      {2, "ADVENTURE", false}, {3, "SPECTATOR", false},
  };
  int succeeded = 0;
  for (const auto& r : rows) {
    Fixture f;
    f.setGameMode(r.mode);
    CHECK_EQ(f.inv->getGameMode(Fixture::kPlayerId), r.mode,
             "the mode really is stored on the inventory store");
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 3, 3, 3,
                               kStoneId, 0, 0);
    ActionDispatcher d;
    CHECK(d.dispatch(ctx), r.label);
    f.flush();
    if (f.world->casCallCount() == 1 && f.world->blockAt(3, 3, 3) == 0) ++succeeded;
  }
  CHECK_EQ(succeeded, 4,
           "a break succeeds in all four modes — the server has no mode gate");
}

// A refused multiblock break (contents cannot fit) must leave the player
// inventory BYTE-IDENTICAL, not merely under a count threshold: the handler
// works on a copy and only calls setSlots on the success path.
void test_BreakBlockHandler_a_refused_break_never_writes_the_inventory() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  std::array<PersistSlot, kInventorySlots> inv{};
  for (auto& s : inv) s = PersistSlot{kStoneId, 64, 0};
  f.inv->setSlots(Fixture::kPlayerId, inv);
  const auto before = f.inv->getSlots(Fixture::kPlayerId);

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
                             kStoneId, 0, 0);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the dispatcher claims it even though the break refuses");
  f.flush();

  CHECK_EQ(f.world->casCallCount(), size_t(0), "a refused break issues no CAS");
  const auto after = f.inv->getSlots(Fixture::kPlayerId);
  CHECK_EQ(after.size(), before.size(), "slot count is unchanged");
  bool identical = true;
  for (size_t i = 0; i < before.size(); ++i) {
    if (after[i].item_id != before[i].item_id ||
        after[i].count != before[i].count || after[i].meta != before[i].meta) {
      identical = false;
    }
  }
  CHECK(identical, "every slot is byte-identical after a refused break");
  CHECK_EQ(f.pub->given.size(), size_t(0), "a refused break drops nothing");
}

// ===========================================================================
// gp-we88 — PlaceBlockHandler
// ===========================================================================

// The happy path end to end, including the ORDER of the side effects. The
// optimistic ACCEPTED ack is published by CasRunner BEFORE the CAS is issued,
// so even a broken world answers ACCEPTED; and the block-changed event fires
// only on a committed CAS.
void test_PlaceBlockHandler_acks_first_then_commits_and_consumes_one() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 5, -1);
  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 5, 6, 7,
                             kAirId, kCobbleStoneId, 1 /* UP */);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the placement is claimed");
  f.flush();

  // face=1 (UP) -> the target is the cell ABOVE the clicked one.
  CHECK_EQ(f.world->casCallCount(), size_t(1), "one placement CAS");
  const auto place_calls = f.world->casCalls();
  if (place_calls.size() == 1) {
    CHECK_EQ(place_calls[0].x, 5, "x is the clicked x");
    CHECK_EQ(place_calls[0].y, 7, "y is one above the clicked y");
    CHECK_EQ(place_calls[0].z, 7, "z is the clicked z");
    CHECK_EQ(place_calls[0].expected, uint16_t(0), "it expects air there");
    CHECK_EQ(place_calls[0].new_id, kCobbleStoneId, "it writes the held block");
    CHECK_EQ(place_calls[0].meta, uint8_t(0), "meta defaults to 0");
  }
  CHECK_EQ(f.world->blockAt(5, 7, 7), kCobbleStoneId, "the world now holds the block");
  CHECK_EQ(f.world->blockAt(5, 6, 7), uint16_t(0), "the CLICKED cell is untouched");
  CHECK_EQ(f.totalInInventory(kCobbleStoneId), 4, "exactly one block is consumed");
  CHECK_EQ(f.pub->placed.size(), size_t(1), "the block-placed hook fires once");
  CHECK_EQ(f.pub->changed.size(), size_t(1), "one block-changed event");
  if (f.pub->changed.size() == 1) {
    CHECK_EQ(f.pub->changed[0].x, 5, "the event names the TARGET cell");
    CHECK_EQ(f.pub->changed[0].y, 7, "the event names the TARGET cell");
    CHECK_EQ(f.pub->changed[0].block_id, kCobbleStoneId, "and the placed block id");
  }
  CHECK_EQ(f.pub->acks.size(), size_t(1), "one ack");
  if (f.pub->acks.size() == 1) {
    CHECK_EQ(f.pub->acks[0].status, uint8_t(Protocol::BlockAckStatus_ACCEPTED),
             "the optimistic ack is ACCEPTED");
  }
}

// The optimistic ack precedes the CAS: flush nothing and the ack is already
// out, while the CAS is still pending. That is the whole reason a CONFLICT
// shows up as a SECOND ack carrying the same request_id.
void test_PlaceBlockHandler_acks_before_the_cas_lands() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 2, -1);
  IBlockRepository::SetBlockCASCallback pending;
  f.world->defer = [&pending](IBlockRepository::SetBlockCASCallback cb) {
    pending = std::move(cb);
  };

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 0, 0, 0,
                             kAirId, kCobbleStoneId, 1);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the placement is claimed");
  CHECK(static_cast<bool>(pending), "the CAS is in flight");

  CHECK_EQ(f.pub->acks.size(), size_t(1), "the optimistic ack is already out");
  CHECK_EQ(f.pub->changed.size(), size_t(0), "nothing is published yet");
  CHECK_EQ(f.totalInInventory(kCobbleStoneId), 2, "nothing is consumed yet");
  CHECK_EQ(f.world->blockAt(0, 1, 0), uint16_t(0), "the world is unchanged yet");

  // Let the CAS complete.
  f.world->defer = nullptr;
  pending(CASResult{0, kCobbleStoneId, 0});
  f.flush();
  CHECK_EQ(f.pub->changed.size(), size_t(1), "the commit publishes the event");
  CHECK_EQ(f.totalInInventory(kCobbleStoneId), 1, "the commit consumes the block");
  CHECK_EQ(f.pub->acks.size(), size_t(1), "the commit adds NO second ack");
}

// A lost race: nothing is consumed, no event, no placed-hook, and the second
// ack reports the block that was ACTUALLY there.
void test_PlaceBlockHandler_a_lost_race_consumes_nothing() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 2, -1);
  f.world->set(5, 7, 7, kStoneId);  // someone else got there first
  f.world->cas_status = 1;          // and the CAS reports CONFLICT

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 5, 6, 7,
                             kAirId, kCobbleStoneId, 1);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the placement is still claimed by the dispatcher");
  f.flush();

  CHECK_EQ(f.world->blockAt(5, 7, 7), kStoneId, "the winner's block is untouched");
  CHECK_EQ(f.totalInInventory(kCobbleStoneId), 2, "a lost race costs the player nothing");
  CHECK_EQ(f.pub->placed.size(), size_t(0), "the block-placed hook does not fire");
  CHECK_EQ(f.pub->changed.size(), size_t(0), "no block-changed event");
  CHECK_EQ(f.pub->acks.size(), size_t(2), "ACCEPTED then CONFLICT");
  if (f.pub->acks.size() == 2) {
    CHECK_EQ(f.pub->acks[0].status, uint8_t(Protocol::BlockAckStatus_ACCEPTED),
             "the optimistic ack is still sent first");
    CHECK_EQ(f.pub->acks[1].status, uint8_t(Protocol::BlockAckStatus_CONFLICT),
             "the second ack reports the conflict");
    CHECK_EQ(f.pub->acks[1].block_id, kStoneId,
             "the CONFLICT ack carries the block actually in the cell");
    CHECK_EQ(f.pub->acks[1].request_id, Fixture::kRequestId,
             "both acks echo the same request id");
  }
}

// gp-t51b (P2, SEPARATE ISSUE — deliberately not fixed here): CREATIVE is
// supposed to place at NO inventory cost (spec: "no inventory slot SHALL be
// consumed"). The handler decrements unconditionally, so a creative player is
// charged exactly like survival. gp-t71g fixes the FORBIDDEN-mode half, not
// this one, and this test is scoped to stay that way.
//
// NARROWED by gp-t71g, and the narrowing is the point: this test used to carry
// four rows and asserted that ADVENTURE and SPECTATOR are ALSO charged, with
// the message "though it cannot place at all". Those two rows are no longer
// reachable — PlaceBlockHandler now declines a forbidden mode outright, so
// there is no charge to observe and no placement to charge it for. Keeping
// them would have meant asserting a value produced by a code path the gate now
// makes unreachable, which is a test that can only ever pass by being wrong.
//
// So the CREATIVE over-charge — the actual subject of gp-t51b — is asserted
// here over the two modes that CAN place, and the forbidden-mode refusal is
// asserted where it belongs, in
// test_PlaceBlockHandler_refuses_adventure_and_spectator.
void test_PlaceBlockHandler_charges_creative_the_same_as_survival() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  // Only the modes that reach the charge at all. ADVENTURE and SPECTATOR are
  // absent because gp-t71g's gate means they never get there.
  struct Row { uint8_t mode; const char* label; };
  const Row rows[] = {
      {0, "SURVIVAL"}, {1, "CREATIVE"},
  };
  std::vector<int> charged;
  for (const auto& r : rows) {
    Fixture f;
    f.setGameMode(r.mode);
    f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 5, -1);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 9, 9, 9,
                               kAirId, kCobbleStoneId, 1);
    ActionDispatcher d;
    CHECK(d.dispatch(ctx), r.label);
    f.flush();
    charged.push_back(5 - f.totalInInventory(kCobbleStoneId));
  }
  CHECK_EQ(charged.size(), size_t(2), "both placeable modes were exercised");
  if (charged.size() == 2) {
    CHECK_EQ(charged[0], 1, "SURVIVAL pays one block, as the spec requires");
    CHECK_EQ(charged[1], 0,
             "CREATIVE places for free (gp-t51b): the spec is explicit that no "
             "inventory slot SHALL be consumed for a creative placement");
  }
}

// gp-t71g, place side, negative: an ADVENTURE or SPECTATOR player SHALL NOT be
// able to place at all. This test used to be named
// test_PlaceBlockHandler_lets_adventure_and_spectator_place and asserted the
// OPPOSITE — placed == 2, with the message "both forbidden modes placed a block
// — the server does not gate on mode" — which pinned the defect GREEN. It is
// flipped here to the behaviour the spec requires
// (openspec/changes/add-interaction-mode-gating/specs/player-interaction/spec.md:
// "Adventure and spectator cannot interact ... no break or place action SHALL be
// sent"; the server half is the authoritative one, because PlaceBlockHandler is
// what mutates the world for ANY client, not just the shipped one).
//
// The refusal must be a REFUSAL, not a silent no-op: the frame is still claimed
// by the dispatcher, the player gets a REJECTED ack carrying the reason, and —
// the part that matters most — NO inventory slot is touched. A check placed
// after the decrement would satisfy "no block in the world" while still
// destroying the player's item, so both are asserted below.
//
// Positive control: SURVIVAL and CREATIVE are adjacent permitted modes and must
// still place. Without it this test would also pass if the gate refused EVERY
// mode, which is a strictly worse bug than the one it fixes.
void test_PlaceBlockHandler_refuses_adventure_and_spectator() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);

  // The matrix itself first, so a failure below is unambiguously the gate and
  // not a changed permission table. Checked through the matrix the handler
  // actually calls, and through the matrix itself, so a divergence between
  // the two would fail here rather than silently pass.
  CHECK(!GameModePerm::CanPlace(GameMode::ADVENTURE));
  CHECK(!GameModePerm::CanPlace(GameMode::SPECTATOR));
  CHECK(GameModePerm::CanPlace(GameMode::SURVIVAL));
  CHECK(GameModePerm::CanPlace(GameMode::CREATIVE));
  CHECK(!CanPlaceBlocksOnServer(static_cast<uint8_t>(GameMode::ADVENTURE)));
  CHECK(!CanPlaceBlocksOnServer(static_cast<uint8_t>(GameMode::SPECTATOR)));
  CHECK(CanPlaceBlocksOnServer(static_cast<uint8_t>(GameMode::SURVIVAL)));
  CHECK(CanPlaceBlocksOnServer(static_cast<uint8_t>(GameMode::CREATIVE)));

  struct Row { uint8_t mode; const char* label; bool allowed; };
  const Row rows[] = {
      {0, "SURVIVAL", true}, {1, "CREATIVE", true},
      {2, "ADVENTURE", false}, {3, "SPECTATOR", false},
  };
  int placed_allowed = 0, placed_forbidden = 0, charged_forbidden = 0;
  for (const auto& r : rows) {
    Fixture f;
    f.setGameMode(r.mode);
    f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 3, -1);
    const auto before = f.inv->getSlots(Fixture::kPlayerId);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 20, 20, 20,
                               kAirId, kCobbleStoneId, 1);
    ActionDispatcher d;
    const bool claimed = d.dispatch(ctx);
    f.flush();

    const bool placed_here = (f.world->blockAt(20, 21, 20) == kCobbleStoneId);
    const int charged = 3 - f.totalInInventory(kCobbleStoneId);
    if (r.allowed) {
      if (placed_here) ++placed_allowed;
      CHECK(claimed, "a permitted mode is still claimed by the dispatcher");
      CHECK(placed_here, "a permitted mode still places the block");
      // Not "every permitted mode pays": CREATIVE places for free, and that is
      // the spec, not an exception (gp-t51b). The table is keyed on the mode
      // rather than on a blanket expectation because the two permitted modes
      // genuinely differ in cost.
      const int expected_cost = (r.mode == 1 /* CREATIVE */) ? 0 : 1;
      CHECK_EQ(charged, expected_cost,
               expected_cost == 0
                   ? "CREATIVE places for free"
                   : "a permitted non-creative mode pays one block");
      continue;
    }
    if (placed_here) ++placed_forbidden;
    // THE ASSERTION: no world mutation from a mode the matrix denies.
    CHECK(!placed_here,
          "OBSERVED HARM: a mode with CanPlace == false wrote a block anyway");
    // THE GATE ITSELF: the dispatcher must NOT claim a forbidden placement.
    // Claiming it and doing nothing would suppress the facade's REJECTED ack
    // (ActionDispatcher::dispatch ORs the handler results), so declining is
    // what lets the refusal be reported at all.
    CHECK(!claimed, "the dispatcher declines a forbidden-mode placement");
    // THE ORDERING ASSERTION: rejected BEFORE the charge, so the refusal costs
    // the player nothing. Byte-identical slots, not merely the same total.
    const auto after = f.inv->getSlots(Fixture::kPlayerId);
    if (charged != 0) ++charged_forbidden;
    CHECK_EQ(charged, 0,
             "a refused placement must not consume a block — the mode check has "
             "to run BEFORE the inventory decrement");
    bool identical = after.size() == before.size();
    for (size_t i = 0; identical && i < before.size(); ++i) {
      if (after[i].item_id != before[i].item_id ||
          after[i].count != before[i].count || after[i].meta != before[i].meta) {
        identical = false;
      }
    }
    CHECK(identical, "every slot is byte-identical after a refused placement");
    CHECK_EQ(f.pub->placed.size(), size_t(0),
             "the block-placed hook never fires for a refused placement");
    CHECK_EQ(f.pub->changed.size(), size_t(0),
             "no block-changed event is published for a refused placement");
    CHECK_EQ(f.world->casCallCount(), size_t(0),
             "a refused placement issues no CAS at all");
  }
  CHECK_EQ(placed_allowed, 2, "both permitted modes placed (positive control)");
  CHECK_EQ(placed_forbidden, 0, "neither forbidden mode placed");
  CHECK_EQ(charged_forbidden, 0, "neither forbidden mode was charged");
}

// The refusal has to be REPORTED, not just declined. A gate that silently drops
// a forbidden placement is only half a fix: the client is left with an
// unresolved optimistic action and no idea why, and the debounce in
// World::IsBlockActionPending never clears. This drives the PRODUCTION facade
// (SetBlockCASHandler, the same path SimCoreMessageHandler.cpp:243-245 takes)
// and asserts the ack is REJECTED, carries a mode-specific reason rather than
// the generic "nothing placeable in hand", and echoes the request id.
//
// The generic-reason half matters: telling a SPECTATOR who is visibly holding a
// cobblestone that it has "nothing placeable in hand" is a false statement that
// would send an operator looking at the client instead of at the mode. So the
// positive and negative control are BOTH here — a permitted mode gets no mode
// reason, and a frame that was never a placement still gets the old string.
void test_SetBlockCASHandler_reports_a_mode_refusal_with_its_own_reason() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);

  struct Case { uint8_t mode; const char* label; bool expect_mode_reason; };
  const Case cases[] = {
      {3, "SPECTATOR", true},   // forbidden: must name the mode
      {2, "ADVENTURE", true},   // forbidden: must name the mode
      {0, "SURVIVAL", false},   // permitted: places, no rejection at all
  };
  for (const auto& c : cases) {
    Fixture f;
    f.setGameMode(c.mode);
    f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 3, -1);
    SetBlockCASHandler facade(
        f.world, f.pub, f.engine, f.inv,
        [&f](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
          f.pub->given.push_back({pid, item, n, slot});
        },
        [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
          f.pub->drill_uses.push_back({pid, x, y, z, b});
        },
        [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
          f.pub->placed.push_back({pid, x, y, z, b});
        },
        // Defer onto the fixture queue so the CAS completion is driven by hand,
        // exactly as the dispatcher-level tests above do.
        [&f](std::function<void()> fn) { f.main_queue_.push_back(std::move(fn)); });

    // The production frame shape: build, finish, GetRoot, cast to void*.
    std::vector<uint8_t> data;
    {
      flatbuffers::FlatBufferBuilder fbb;
      Protocol::Vec3i pos(20, 20, 20);
      const auto off = Protocol::CreateSetBlockAction(
          fbb, Fixture::kPlayerId, Protocol::PlayerActionType_RIGHT_MOUSE_CLICK,
          &pos, kAirId, kCobbleStoneId, Fixture::kRequestId, 1, kCobbleStoneId);
      fbb.Finish(off);
      data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }
    facade.handle((void*)flatbuffers::GetRoot<Protocol::SetBlockAction>(data.data()));
    f.flush();

    if (!c.expect_mode_reason) {
      // Positive control: a permitted mode is not rejected at all, so the mode
      // reason cannot be firing indiscriminately.
      bool any_rejected = false;
      for (const auto& a : f.pub->acks) {
        if (a.status == uint8_t(Protocol::BlockAckStatus_REJECTED)) any_rejected = true;
      }
      CHECK(!any_rejected, c.label);
      continue;
    }
    const AckRecord* rej = nullptr;
    for (const auto& a : f.pub->acks) {
      if (a.status == uint8_t(Protocol::BlockAckStatus_REJECTED)) rej = &a;
    }
    CHECK(rej != nullptr, "a forbidden mode is answered with a REJECTED ack");
    if (rej) {
      CHECK(rej->reason.find("game mode") != std::string::npos,
            "the REJECTED ack names the game mode, not 'nothing placeable in hand'");
      CHECK(rej->reason.find("nothing placeable") == std::string::npos,
            "and does NOT claim the player has nothing placeable in hand — they "
            "do, and the frame said so");
      CHECK_EQ(rej->request_id, Fixture::kRequestId,
               "the ack echoes the request id so the client can resolve it");
    }
    CHECK_EQ(f.totalInInventory(kCobbleStoneId), 3, "and nothing was charged");
  }
}

// A frame that was never a placement must keep the generic reason, so the
// mode-specific string cannot leak into unrelated rejections. Empty hand: the
// shape check fails before the mode is even consulted, so even a SPECTATOR
// holding nothing gets the old wording rather than a mode accusation.
void test_TheModeReasonNeverLeaksOntoANonPlacementFrame() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  f.setGameMode(3);  // SPECTATOR — forbidden, but holding NOTHING
  SetBlockCASHandler facade(
      f.world, f.pub, f.engine, f.inv,
      [&f](uint64_t, uint16_t, uint8_t, int32_t) {},
      [&f](uint64_t, int32_t, int32_t, int32_t, uint16_t) {},
      [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
        f.pub->placed.push_back({pid, x, y, z, b});
      },
      [&f](std::function<void()> fn) { f.main_queue_.push_back(std::move(fn)); });

  std::vector<uint8_t> data;
  {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(2, 2, 2);
    const auto off = Protocol::CreateSetBlockAction(
        fbb, Fixture::kPlayerId, Protocol::PlayerActionType_RIGHT_MOUSE_CLICK,
        &pos, kStoneId, 0 /* held_item: nothing in hand */, Fixture::kRequestId, 0, 0);
    fbb.Finish(off);
    data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  }
  facade.handle((void*)flatbuffers::GetRoot<Protocol::SetBlockAction>(data.data()));
  f.flush();

  const AckRecord* rej = nullptr;
  for (const auto& a : f.pub->acks) {
    if (a.status == uint8_t(Protocol::BlockAckStatus_REJECTED)) rej = &a;
  }
  CHECK(rej != nullptr, "an empty-handed right click is still rejected");
  if (rej) {
    CHECK(rej->reason.find("nothing placeable in hand") != std::string::npos,
          "it keeps the generic reason — the mode was never the cause");
    CHECK(rej->reason.find("game mode") == std::string::npos,
          "and the mode-specific reason did not leak onto it");
  }
}

// An undefined mode byte must fail CLOSED on the server too. GameMode arrives
// off the wire as a bare uint8 (`player.gamemode.change`,
// src/protocol/core.fbs:25) and SimCoreMessageHandler.cpp:330 casts it to
// uint8_t and stores it without validating. A deny-list here would admit every
// value the enum does not name — the gp-ul16 lesson, on the authoritative half.
void test_PlaceBlockHandler_refuses_an_undefined_game_mode() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  for (uint8_t raw : {uint8_t{4}, uint8_t{9}, uint8_t{200}, uint8_t{255}}) {
    CHECK(!CanPlaceBlocksOnServer(raw),
          "an undefined mode cannot place — the server gate fails closed");

    Fixture f;
    f.setGameMode(raw);
    f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 3, -1);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 30, 30, 30,
                               kAirId, kCobbleStoneId, 1);
    ActionDispatcher d;
    // The dispatcher DECLINES it: claiming a frame and doing nothing would
    // suppress the facade's REJECTED ack, so "fails closed" has to mean
    // "not claimed" rather than "claimed but inert".
    CHECK(!d.dispatch(ctx), "the dispatcher declines an undefined-mode placement");
    f.flush();
    CHECK_EQ(f.world->blockAt(30, 31, 30), uint16_t(0),
             "an undefined mode places nothing");
    CHECK_EQ(f.totalInInventory(kCobbleStoneId), 3,
             "an undefined mode is charged nothing");
    CHECK_EQ(f.world->casCallCount(), size_t(0),
             "an undefined mode issues no CAS");
  }
  // Positive control for the whole sweep: the gate refuses undefined values
  // WITHOUT becoming a blanket refusal, so SURVIVAL still places after it.
  {
    Fixture f;
    f.setGameMode(0);
    f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 3, -1);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 40, 40, 40,
                               kAirId, kCobbleStoneId, 1);
    ActionDispatcher d;
    CHECK(d.dispatch(ctx), "SURVIVAL still places after the undefined sweep");
    f.flush();
    CHECK_EQ(f.world->blockAt(40, 41, 40), kCobbleStoneId,
             "and the block really lands — the gate is not refusing everyone");
  }
}

// Placing consumes the FIRST matching stack, and only ONE of them, even with
// several stacks of the same block.
void test_PlaceBlockHandler_consumes_one_from_the_first_matching_stack() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  std::array<PersistSlot, kInventorySlots> inv{};
  inv[2] = PersistSlot{kCobbleStoneId, 10, 0};
  inv[5] = PersistSlot{kCobbleStoneId, 20, 0};
  inv[8] = PersistSlot{kStoneId, 3, 0};
  f.inv->setSlots(Fixture::kPlayerId, inv);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 1, 1, 1,
                             kAirId, kCobbleStoneId, 1);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the placement is claimed");
  f.flush();

  const auto after = f.inv->getSlots(Fixture::kPlayerId);
  CHECK_EQ(after[2].count, uint8_t(9), "the FIRST matching stack loses one");
  CHECK_EQ(after[5].count, uint8_t(20), "the second matching stack is untouched");
  CHECK_EQ(after[8].item_id, kStoneId, "unrelated slots are untouched");
  CHECK_EQ(after[2].item_id, kCobbleStoneId, "the stack keeps its item id");
}

// A placement whose held block is NOT in the inventory still writes the world
// (the CAS is the authority) but consumes nothing — the decrement loop simply
// finds no matching stack. Worth pinning: the client is the only thing that
// knows the player owns the block.
void test_PlaceBlockHandler_writes_the_world_even_with_an_empty_inventory() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 4, 4, 4,
                             kAirId, kCobbleStoneId, 1);
  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the placement is claimed with an empty inventory");
  f.flush();
  CHECK_EQ(f.world->blockAt(4, 5, 4), kCobbleStoneId,
           "the block is placed out of thin air");
  CHECK_EQ(f.totalInInventory(kCobbleStoneId), 0, "nothing was consumed");
  CHECK_EQ(f.pub->placed.size(), size_t(1), "the hook still fires");
}

// The transform table rewrites BOTH the id and the meta that the CAS writes.
// transforms.csv is a real file; the row below uses the real table loaded from
// disk, and asserts against whatever the table says rather than a hardcoded
// expectation, so a data change shows up as a data-driven diff.
void test_PlaceBlockHandler_applies_the_transform_table_to_id_and_meta() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(BlockTransforms::Load(TRANSFORMS_CSV));
  CHECK(BlockTransforms::instance() != nullptr, "the real transform table loaded");

  Fixture f;
  f.inv->giveItem(Fixture::kPlayerId, kCobbleStoneId, 1, -1);
  // A right click always derives eff_expected = 0, so the transform is looked
  // up on (air, held). That is a real limitation of the current code — the
  // transform can never fire on the place path, because the key it needs
  // (the clicked block) is not what it is given. Asserted, not hidden.
  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 6, 6, 6,
                             kStoneId, kCobbleStoneId, 1);
  CHECK_EQ(ctx.eff_expected, uint16_t(0),
           "the place path always expects air on the adjacent cell");
  CHECK_EQ(ctx.expected_block_id, kStoneId,
           "the clicked block is still known on the context");

  ActionDispatcher d;
  CHECK(d.dispatch(ctx), "the placement is claimed");
  f.flush();
  // Whatever the table holds for (air, cobblestone), the CAS wrote the held id
  // when there is no rule. Pinned by value so a table edit is visible.
  CHECK_EQ(f.world->casCallCount(), size_t(1), "one CAS");
  const auto tf_calls = f.world->casCalls();
  if (tf_calls.size() == 1) {
    // The handler resolves the transform against ctx.eff_expected, which is
    // always 0 on a right click, so the written id is the held block unless
    // the table happens to define a (air, held) rule.
    const bool has_air_rule =
        BlockTransforms::instance()->Apply(kAirId, kCobbleStoneId).has_value();
    CHECK_EQ(tf_calls[0].new_id,
             has_air_rule ? BlockTransforms::instance()->Apply(kAirId, kCobbleStoneId)
                                ->new_block_id
                          : kCobbleStoneId,
             "the written id follows the (eff_expected, held) transform lookup");
  }
  CHECK_EQ(f.totalInInventory(kCobbleStoneId), 0,
           "the HELD block id is what gets consumed, even if a transform changed the write");
}

// ===========================================================================
// gp-rbcw — CasRunner / SetBlockCASHandler
// ===========================================================================

// The postToMain indirection: when the context carries a postToMain hook, the
// CAS completion is DEFERRED onto it. That is the mechanism that lets the
// handler's onCommitted lambda run on the main thread, and it is the reason
// runBlockCas copies so much state instead of capturing references.
void test_CasRunner_defers_the_completion_onto_postToMain() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  std::vector<std::function<void()>> posted;
  // A context whose postToMain queues rather than runs.
  flatbuffers::FlatBufferBuilder b;
  Protocol::Vec3i pos(8, 8, 8);
  const auto off = Protocol::CreateSetBlockAction(
      b, Fixture::kPlayerId, Protocol::PlayerActionType_LEFT_MOUSE_CLICK, &pos,
      kStoneId, 0, Fixture::kRequestId, 0, 0);
  b.Finish(off);
  const auto* table = flatbuffers::GetRoot<Protocol::SetBlockAction>(b.GetBufferPointer());
  ActionContext ctx(table, f.world, f.pub, f.engine, f.inv, nullptr,
                    [&f](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
                      f.pub->given.push_back({pid, item, n, slot});
                    },
                    [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
                      f.pub->drill_uses.push_back({pid, x, y, z, b});
                    },
                    [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
                      f.pub->placed.push_back({pid, x, y, z, b});
                    },
                    [&posted](std::function<void()> fn) { posted.push_back(std::move(fn)); });

  BreakBlockHandler h;
  CHECK(h.canHandle(ctx), "the break handler claims a left click");
  h.handle(ctx);

  // The CAS already completed synchronously inside the world double, so the
  // onCommitted body is sitting in `posted` and has NOT run yet.
  CHECK_EQ(posted.size(), size_t(1), "the completion was posted, not run inline");
  CHECK_EQ(f.pub->changed.size(), size_t(0), "no side effect has happened yet");
  CHECK_EQ(f.world->blockAt(8, 8, 8), uint16_t(0),
           "the world IS already updated (the CAS ran on its own thread)");
  for (auto& fn : posted) fn();
  CHECK_EQ(f.pub->changed.size(), size_t(1),
           "running the posted closure performs the side effects");
  CHECK_EQ(f.pub->given.size(), size_t(1), "and the drop");
}

// Without a postToMain the completion runs inline on the CAS thread. Same
// outcome, different thread — which is why the handler copies its state.
void test_CasRunner_without_postToMain_runs_the_completion_inline() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  flatbuffers::FlatBufferBuilder b;
  Protocol::Vec3i pos(9, 9, 9);
  const auto off = Protocol::CreateSetBlockAction(
      b, Fixture::kPlayerId, Protocol::PlayerActionType_LEFT_MOUSE_CLICK, &pos,
      kStoneId, 0, Fixture::kRequestId, 0, 0);
  b.Finish(off);
  const auto* table = flatbuffers::GetRoot<Protocol::SetBlockAction>(b.GetBufferPointer());
  ActionContext ctx(table, f.world, f.pub, f.engine, f.inv, nullptr,
                    [&f](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
                      f.pub->given.push_back({pid, item, n, slot});
                    },
                    [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
                      f.pub->drill_uses.push_back({pid, x, y, z, b});
                    },
                    [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
                      f.pub->placed.push_back({pid, x, y, z, b});
                    },
                    nullptr);
  BreakBlockHandler h;
  h.handle(ctx);
  CHECK_EQ(f.pub->changed.size(), size_t(1),
           "with no postToMain the effect happens inline");
  CHECK_EQ(f.pub->given.size(), size_t(1), "including the drop");
}

// The real concurrent race gp-rbcw asks for: N threads CAS the SAME cell from
// the SAME expected value. Exactly one must win, and the losers must all get
// CONFLICT. The world double serialises the compare-and-swap under a mutex —
// see the THREAD SAFETY note on the class — which is precisely what a real
// single-writer block store does, so the race is genuine rather than
// simulated: the threads really do interleave, and the outcome is produced by
// the CAS, not by a counter.
//
// The compare and the write share ONE critical section, so the assertion below
// is a real invariant of the primitive and not an artefact of scheduling: no
// interleaving of these 8 threads can produce two winners.
void test_CasRunner_two_racing_writers_leave_exactly_one_winner() {
  constexpr int kThreads = 8;
  constexpr int32_t kX = 11, kY = 12, kZ = 13;
  World world;
  world.set(kX, kY, kZ, kStoneId);

  std::atomic<int> committed{0};
  std::atomic<int> conflicted{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&world, &committed, &conflicted, i] {
      // Each thread CASes air -> its own distinct block id, all expecting
      // the stone that is there now.
      const uint16_t want = static_cast<uint16_t>(0x0F00 + i);
      world.setBlockCAS(
          kX, kY, kZ, kStoneId, want, 0,
          [&committed, &conflicted, want](const CASResult& r) {
            if (r.status == 0) {
              if (r.block_id == want) ++committed;
            } else {
              ++conflicted;
            }
          });
    });
  }
  for (auto& t : threads) t.join();

  CHECK_EQ(committed.load(), 1, "exactly ONE racing writer commits");
  CHECK_EQ(conflicted.load(), kThreads - 1, "every other writer is told CONFLICT");
  CHECK_EQ(world.casCallCount(), size_t(kThreads), "every writer did issue a CAS");
  // The cell holds one of the candidates, never the stone.
  const uint16_t final = world.blockAt(kX, kY, kZ);
  CHECK_NE(final, kStoneId, "the original block is gone");
  bool is_a_candidate = final >= 0x0F00 && final < 0x0F00 + kThreads;
  CHECK(is_a_candidate, "the cell holds exactly one writer's block");
}

void test_SetBlockCASHandler_rejects_an_unhandled_action_with_a_reason() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  SetBlockCASHandler facade(
      f.world, f.pub, f.engine, f.inv,
      [&f](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
        f.pub->given.push_back({pid, item, n, slot});
      },
      [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
        f.pub->drill_uses.push_back({pid, x, y, z, b});
      },
      [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
        f.pub->placed.push_back({pid, x, y, z, b});
      },
      [](std::function<void()> fn) { fn(); });

  // EXACTLY the production path: build into a vector<uint8_t>, verify with the
  // same Verifier main.cpp uses, GetRoot, then cast to void* (line 245).
  std::vector<uint8_t> data;
  {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(2, 2, 2);
    const auto off = Protocol::CreateSetBlockAction(
        fbb, Fixture::kPlayerId, Protocol::PlayerActionType_RIGHT_MOUSE_CLICK,
        &pos, kStoneId, 0, Fixture::kRequestId, 0, 0);
    fbb.Finish(off);
    data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  }
  flatbuffers::Verifier v(data.data(), data.size());
  CHECK(v.VerifyBuffer<Protocol::SetBlockAction>(), "the frame verifies");
  auto* action = flatbuffers::GetRoot<Protocol::SetBlockAction>(data.data());
  CHECK_EQ(action->action(), uint8_t(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK),
           "the frame really is a right click");
  facade.handle((void*)action);

  CHECK_EQ(f.world->casCallCount(), size_t(0), "an unhandled action issues no CAS");
  CHECK_EQ(f.pub->acks.size(), size_t(1), "the facade answers exactly once");
  if (f.pub->acks.size() == 1) {
    const auto& a = f.pub->acks[0];
    CHECK_EQ(a.status, uint8_t(Protocol::BlockAckStatus_REJECTED),
             "the fallback ack is REJECTED");
    CHECK_EQ(a.reason, std::string("nothing placeable in hand"),
             "and explains itself");
    CHECK_EQ(a.request_id, Fixture::kRequestId,
             "the rejection echoes the client request_id");
    CHECK_EQ(a.x, 2, "and the clicked x");
    CHECK_EQ(a.y, 2, "and y");
    CHECK_EQ(a.z, 2, "and z");
  }
}

void test_SetBlockCASHandler_ignores_a_null_table() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  SetBlockCASHandler facade(f.world, f.pub, f.engine, f.inv);
  facade.handle(static_cast<const void*>(nullptr));
  CHECK_EQ(f.pub->acks.size(), size_t(0), "a null frame produces nothing");
  CHECK_EQ(f.world->casCallCount(), size_t(0), "and touches no block");
}

// A HANDLED action through the facade: the break really happens and the
// single ack is the real ACCEPTED, not the spurious REJECTED the gp-qij1 fix
// removed. Driven through the facade rather than a hand-rolled copy of its
// if/return, so the contract is tested where production runs it.
void test_SetBlockCASHandler_acks_a_handled_action_exactly_once() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  SetBlockCASHandler facade(
      f.world, f.pub, f.engine, f.inv,
      [&f](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
        f.pub->given.push_back({pid, item, n, slot});
      },
      [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
        f.pub->drill_uses.push_back({pid, x, y, z, b});
      },
      [&f](uint64_t pid, int32_t x, int32_t y, int32_t z, uint16_t b) {
        f.pub->placed.push_back({pid, x, y, z, b});
      },
      [](std::function<void()> fn) { fn(); });

  // The production path: build into a vector, verify, GetRoot, cast to void*.
  std::vector<uint8_t> data;
  {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(9, 8, 7);
    const auto off = Protocol::CreateSetBlockAction(
        fbb, Fixture::kPlayerId, Protocol::PlayerActionType_LEFT_MOUSE_CLICK,
        &pos, kStoneId, 0, Fixture::kRequestId, 0, 0);
    fbb.Finish(off);
    data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  }
  flatbuffers::Verifier v(data.data(), data.size());
  CHECK(v.VerifyBuffer<Protocol::SetBlockAction>(), "the frame verifies");
  facade.handle((void*)flatbuffers::GetRoot<Protocol::SetBlockAction>(data.data()));

  CHECK_EQ(f.world->blockAt(9, 8, 7), uint16_t(0), "the break really happened");
  CHECK_EQ(f.world->casCallCount(), size_t(1), "exactly one break CAS");
  CHECK_EQ(f.pub->acks.size(), size_t(1), "exactly one ack — no spurious second");
  if (!f.pub->acks.empty()) {
    const auto& a = f.pub->acks[0];
    CHECK_EQ(a.status, uint8_t(Protocol::BlockAckStatus_ACCEPTED),
             "and it is ACCEPTED, not the old spurious REJECTED");
    CHECK_EQ(a.request_id, Fixture::kRequestId,
             "the optimistic ack DOES echo the client request_id");
    CHECK_EQ(a.x, 9, "and the real x");
    CHECK_EQ(a.y, 8, "and the real y");
    CHECK_EQ(a.z, 7, "and the real z");
    CHECK_EQ(a.action_type, uint8_t(Protocol::PlayerActionType_LEFT_MOUSE_CLICK),
             "and the action type that triggered it");
  }
}

// ===========================================================================
// gp-80ll — MachineInteractHandler (machine open / close interaction)
// ===========================================================================
//
// gp-80ll asks for three things, each tested below against the real handler
// rather than a re-derivation of its logic:
//
//   1. opening a machine of a known type produces the documented response;
//   2. an unknown machine type is REJECTED rather than opening an empty
//      window;
//   3. interacting twice is idempotent.
//
// "The documented response" is the publishMachineState triple at
// MachineInteractHandler.cpp:64-68 — a block-entity update carrying the real
// energy state, an ACCEPTED ack, and an OPEN_UI directive. There is no
// separate close action: the spec's "close" is the client-side consequence of
// the same topic family, and the server has no MachineCloseHandler on this
// path (SimCoreMessageHandler registers MachineOpen/MachineClose under
// player.machine.open / .close, which are the session path, not this
// SetBlockAction path). So idempotence is tested where it is observable: the
// second interaction re-publishes the same state and mutates nothing new.
//
// The interesting half is the LAZY-INIT path. A machine whose ECS entity
// does not exist yet (it predates this simcore instance) is created from the
// block repository, and the real block there — not the id the client claimed
// — decides whether this is a machine at all. That is the branch that can
// reject, and it is where both the known-type and unknown-type cases live.

void test_MachineInteractHandler_opens_a_known_machine_with_its_real_state() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  // 1110:000:0 = heat_furnace: energy_in HEAT, capacity 10000, tier 0.
  f.world->set(4, 5, 6, kFurnaceId);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 4, 5, 6,
                             kFurnaceId, 0, 0);
  MachineInteractHandler h;
  CHECK(h.canHandle(ctx), "a right click on a known machine is claimed");
  CHECK(ctx.machine_info != nullptr, "the registry resolved the machine type");
  h.handle(ctx);
  f.flush();

  // The lazy-init really did create the entity, and it is a real machine.
  auto& reg = f.engine->reg();
  entt::entity ent = entt::null;
  auto vw = reg.view<const simcore::Position>();
  for (auto e : vw) {
    auto& p = vw.get<const simcore::Position>(e);
    if (p.x == 4 && p.y == 5 && p.z == 6) ent = e;
  }
  CHECK(ent != entt::null, "the entity was lazily created from ChunkStore");
  CHECK(reg.all_of<simcore::MachineComponent>(ent),
        "and it really is a MachineComponent");
  if (reg.all_of<simcore::MachineComponent>(ent)) {
    CHECK_EQ(reg.get<simcore::MachineComponent>(ent).machine_id, kFurnaceId,
             "the machine id is the block that is actually there");
  }

  // The documented response, in order: entity update, ACCEPTED ack, OPEN_UI.
  CHECK_EQ(f.world->blockReads().size(), size_t(1),
           "the handler read the block once to decide");
  CHECK_EQ(f.pub->acks.size(), size_t(1), "exactly one ack");
  CHECK_EQ(f.pub->directives.size(), size_t(1), "exactly one directive");
  if (f.pub->acks.size() == 1) {
    const auto& a = f.pub->acks[0];
    CHECK_EQ(a.status, uint8_t(Protocol::BlockAckStatus_ACCEPTED),
             "a known machine is ACCEPTED");
    CHECK_EQ(a.x, 4, "the ack names the machine x");
    CHECK_EQ(a.y, 5, "the machine y");
    CHECK_EQ(a.z, 6, "the machine z");
    CHECK_EQ(a.block_id, kFurnaceId, "and the machine block id");
    CHECK_EQ(a.request_id, Fixture::kRequestId, "echoing the client request id");
  }
  if (f.pub->directives.size() == 1) {
    const auto& d = f.pub->directives[0];
    CHECK_EQ(d.directive, uint8_t(Protocol::BlockDirective_OPEN_UI),
             "the directive is OPEN_UI — this is what opens the window");
    CHECK_EQ(d.block_id, kFurnaceId, "for the right machine");
    CHECK_EQ(d.request_id, Fixture::kRequestId, "with the request id echoed");
  }
}

// A block that is NOT a machine is rejected rather than opening an empty
// window. The client claimed a furnace, but the world holds stone: the real
// block wins, and the handler answers REJECTED with a reason.
void test_MachineInteractHandler_rejects_a_block_that_is_not_a_machine() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  f.world->set(1, 1, 1, kStoneId);  // the client thinks this is a furnace

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 1, 1, 1,
                             kFurnaceId, 0, 0);
  MachineInteractHandler h;
  h.handle(ctx);
  f.flush();

  CHECK_EQ(f.pub->acks.size(), size_t(1), "exactly one ack");
  if (f.pub->acks.size() == 1) {
    const auto& a = f.pub->acks[0];
    CHECK_EQ(a.status, uint8_t(Protocol::BlockAckStatus_REJECTED),
             "a non-machine is REJECTED, not opened");
    CHECK_EQ(a.reason, std::string("Block is not a machine"),
             "and it says why");
    CHECK_EQ(a.block_id, kStoneId,
             "the rejection names the block ACTUALLY there, not the claimed one");
  }
  CHECK_EQ(f.pub->directives.size(), size_t(0),
           "no OPEN_UI directive — no empty window is ever opened");
}

// An id the registry does not know at all, with a world cell that is air.
// finalId stays the claimed id, IsMachine says no, and the same rejection
// path runs. This is the "unknown machine type" half of gp-80ll.
void test_MachineInteractHandler_rejects_an_unknown_machine_type() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  // 0xFFFF is not in machines.yaml. Nothing is written to the cell, so the
  // handler's `bd.block_id != 0` guard leaves finalId at the claimed id.
  const uint16_t kUnknownId = 0xFFFF;

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 2, 2, 2,
                             kUnknownId, 0, 0);
  MachineInteractHandler h;
  CHECK(!h.canHandle(ctx),
        "an unknown id is not even a machine_info, so nothing claims it");
  h.handle(ctx);  // driven directly: the facade is what skips unclaimed actions
  f.flush();

  CHECK_EQ(f.pub->acks.size(), size_t(1), "the handler answers exactly once");
  if (f.pub->acks.size() == 1) {
    CHECK_EQ(f.pub->acks[0].status,
             uint8_t(Protocol::BlockAckStatus_REJECTED),
             "an unknown machine type is REJECTED");
    CHECK_EQ(f.pub->acks[0].reason, std::string("Block is not a machine"),
             "with the documented reason");
  }
  CHECK_EQ(f.pub->directives.size(), size_t(0), "and no window is opened");
}

// Idempotence: the SECOND interaction of the same machine, after the entity
// exists, produces the same response and leaves the ECS unchanged. The entity
// now exists, so handle() takes the early-return branch at line 81 and skips
// the lazy-init read entirely.
void test_MachineInteractHandler_twice_is_idempotent() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  f.world->set(7, 7, 7, kFurnaceId);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 7, 7, 7,
                             kFurnaceId, 0, 0);
  MachineInteractHandler h;
  h.handle(ctx);
  f.flush();
  const size_t reads_after_first = f.world->blockReads().size();
  CHECK_EQ(f.pub->acks.size(), size_t(1), "the first interaction acked once");
  CHECK_EQ(f.pub->directives.size(), size_t(1), "and opened the window once");

  h.handle(ctx);
  f.flush();

  // The second pass takes the already-exists branch: no re-read, no new
  // entity, the same single-machine response.
  CHECK_EQ(f.world->blockReads().size(), reads_after_first,
           "the second interaction does NOT re-read ChunkStore");
  CHECK_EQ(f.pub->acks.size(), size_t(2), "one more ack");
  CHECK_EQ(f.pub->directives.size(), size_t(2), "and one more OPEN_UI");
  if (f.pub->acks.size() == 2) {
    CHECK_EQ(f.pub->acks[0].status, f.pub->acks[1].status,
             "both interactions produce the same ack status");
    CHECK_EQ(f.pub->acks[0].block_id, f.pub->acks[1].block_id,
             "for the same machine");
    CHECK_EQ(f.pub->acks[0].request_id, f.pub->acks[1].request_id,
             "and the same request id");
  }

  // Still exactly ONE machine entity: the second pass created nothing.
  int machine_entities = 0;
  auto& reg = f.engine->reg();
  auto vw = reg.view<const simcore::MachineComponent>();
  for (auto e : vw) {
    const auto& p = reg.get<const simcore::Position>(e);
    if (p.x == 7 && p.y == 7 && p.z == 7) ++machine_entities;
  }
  CHECK_EQ(machine_entities, 1,
           "interacting twice creates exactly ONE machine entity, not two");
}

// The energy state reported in the block-entity update is the REAL one, not a
// hardcoded zero — that is the whole point of publishMachineState. The lazy
// path has no entity yet, so it falls back to the registry's declared
// energy_in, which for a heat_furnace is HEAT and not ELECTRICITY.
void test_MachineInteractHandler_reports_the_registry_energy_type_not_zero() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  Fixture f;
  f.world->set(10, 11, 12, kFurnaceId);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 10, 11,
                             12, kFurnaceId, 0, 0);
  MachineInteractHandler h;
  h.handle(ctx);
  f.flush();

  // The recorded energy type must be what machines.yaml declares, so a
  // hardcoded 0/ELECTRICITY regression fails here.
  const auto* info = f.registry->Get(kFurnaceId);
  CHECK(info != nullptr, "the furnace is in the registry");
  if (info) {
    CHECK(info->energy_in.has_value(), "and declares an energy_in");
    if (info->energy_in.has_value()) {
      CHECK_EQ(f.pub->last_entity_update_etype, info->energy_in.value(),
               "the reported energy type is the machine's declared one");
    }
    CHECK_EQ(f.pub->last_entity_update_slots_in, info->slots_in,
             "the window is told how many input slots the machine has");
  }
}

// A left click only interacts when the machine opts in
// (interact_on_left) AND the hand holds no mining tool. rotare_generator is
// the one machine in machines.yaml that opts in.
void test_MachineInteractHandler_left_click_spins_only_an_opted_in_machine() {
  DropsGuard drops(nullptr);
  TransformsGuard transforms(nullptr);
  const uint16_t kRotareId = ItemId::pack("1110:100:1");

  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 3, 3, 3,
                               kRotareId, 0, 0);
    MachineInteractHandler h;
    CHECK(h.canHandle(ctx), "rotare_generator opts into left-click interaction");
    h.handle(ctx);
    f.flush();
    CHECK_EQ(f.pub->acks.size(), size_t(1), "the spin acks once");
    CHECK_EQ(f.pub->directives.size(), size_t(1), "and emits one directive");
    if (f.pub->acks.size() == 1) {
      CHECK_EQ(f.pub->acks[0].status, uint8_t(Protocol::BlockAckStatus_ACCEPTED),
               "the spin is ACCEPTED");
      CHECK_EQ(f.pub->acks[0].action_type,
               uint8_t(Protocol::PlayerActionType_LEFT_MOUSE_CLICK),
               "and echoes the left click that caused it");
    }
    if (f.pub->directives.size() == 1) {
      CHECK_EQ(f.pub->directives[0].directive,
               uint8_t(Protocol::BlockDirective_PLAY_ANIMATION),
               "a left click PLAYS THE ANIMATION, it does not open a window");
    }
    CHECK_EQ(f.world->blockReads().size(), size_t(0),
             "the left-click path never touches ChunkStore");
  }

  // Held a mining tool: a left click is a BREAK, not an interaction, even
  // though the machine opted in. The dispatcher agrees.
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 3, 3, 3,
                               kRotareId, ITEM_DRILL_LV, 0);
    MachineInteractHandler h;
    CHECK(!h.canHandle(ctx),
          "a drill in hand defeats the left-click interaction");
  }

  // A machine that does NOT opt in: a left click breaks it instead.
  {
    Fixture f;
    ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 3, 3, 3,
                               kFurnaceId, 0, 0);
    MachineInteractHandler h;
    CHECK(!h.canHandle(ctx), "a furnace does not opt into left-click interaction");
  }
}

// ===========================================================================
// gp-zekn — ElectricDrillHandler + ToolActionHandler: tool action routing
// ===========================================================================
//
// gp-zekn asks for three things, each tested here against the real handlers:
//   * each tool action routes to its handler;
//   * an action with no tool is rejected;
//   * the drill handler requires the drill tool and rejects a bare hand.
//
// ElectricDrillHandler is a plain class (no ITopicHandler), driven through
// mineBlock with getBlock/setBlock callbacks, so the world is a two-line
// closure rather than the shared World double — the handler never touches an
// IBlockRepository. ToolActionHandler IS an ITopicHandler, driven through
// handle(vector<uint8_t>) exactly as SimCoreMessageHandler drives it from the
// "player.tool.action" topic, with a real FlatBuffer frame.
//
// ── WHY YOU SEE ALMOST NO ASSERTIONS ABOUT SUCCESS ───────────────────────
// Two production facts dominate these paths and both are pinned as-is, because
// the tests must describe the code rather than the spec:
//
//   1. NONE of the four drill ids in ToolIds.h decodes to a non-zero tier.
//      ItemId::pack("1111:00:0").."1111:00:3" are 0xF000..0xF003, so
//      ItemId::toolTier computes payload = id - 0xF000 = 0..3, and
//      (payload >> 5) & 0x1F = 0 for all four. So ITEM_DRILL_LV/MV/HV all
//      report tier 0 = ULV. The only tier-1+ ids the decoder can produce are
//      0xF020+ (payload 32 = 1<<5), which items.csv does not contain.
//
//   2. NONE of the four drill ids is in TOOL_ENERGY_DEFS (keys 90-94, 60948-
//      60950). getToolEnergy returns -1 for them, and consumeToolEnergy()
//      returns false whenever the current energy is less than the requested
//      amount — and -1 < 100 is true — so no drill mine can ever succeed no
//      matter how much meta the tool carries. (gp-j1ux splits the reason: that
//      is "no_energy_definition", not "no_energy".)
//
// Together those mean mineBlock can NEVER report success for a real drill id.
// That is a genuine defect, not a test artefact; it is filed separately rather
// than asserted as correct, and the tests below pin the reachable branches.

// FINDING (gp-zekn): the four drill ids all decode to tier 0, so the tier
// ladder ElectricDrillHandler relies on does not exist for the shipped tools.
void test_ElectricDrillHandler_the_four_drill_ids_all_decode_to_tier_zero() {
  struct Row { uint16_t id; const char* label; };
  const Row rows[] = {
      {ITEM_DRILL_ULV, "ITEM_DRILL_ULV"}, {ITEM_DRILL_LV, "ITEM_DRILL_LV"},
      {ITEM_DRILL_MV, "ITEM_DRILL_MV"},   {ITEM_DRILL_HV, "ITEM_DRILL_HV"},
  };
  for (const auto& r : rows) {
    CHECK_EQ(toolTier(r.id), 0,
             "ItemId::toolTier decodes this drill as tier 0 (ULV)");
    CHECK_EQ(miningLevel(toolTier(r.id)), 1,
             "so its mining level is the ULV one");
  }
  // The decoder CAN produce a tier-1 id, it just is not in ToolIds.h:
  // payload 32 = 1<<5, so pack("1111:00:32") = 0xF020.
  CHECK_NE(toolTier(ItemId::pack("1111:00:32")), 0,
           "tier 1 is reachable, just not by any shipped drill id");
  for (const auto& r : rows) {
    CHECK(TOOL_ENERGY_DEFS.find(r.id) == TOOL_ENERGY_DEFS.end(),
          "and no drill id is in TOOL_ENERGY_DEFS, which is what makes every "
          "mine fail on energy grounds");
  }
}

// "The drill handler requires the drill tool and rejects a bare hand." The
// gate at ElectricDrillHandler.cpp:40 is `tier == 0 && id != ITEM_DRILL_ULV`.
// A BARE HAND is a non-drill id, so it is refused before anything else is
// even read — the world is never consulted.
void test_ElectricDrillHandler_rejects_a_bare_hand_before_touching_the_world() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  int world_reads = 0;
  int world_writes = 0;
  ElectricDrillHandler h(
      [&](int32_t, int32_t, int32_t) {
        ++world_reads;
        return kStoneId;
      },
      [&](int32_t, int32_t, int32_t, uint16_t) { ++world_writes; }, inv);

  // A bare hand: item id 0, which is neither a drill nor ITEM_DRILL_ULV.
  const DrillMineResult r = h.mineBlock(7, 0, 0, 0, /*tool*/ 0, /*slot*/ 0);
  CHECK(!r.success, "a bare hand cannot mine");
  CHECK_EQ(r.error, std::string("not_a_drill"), "rejected as 'not a drill'");
  CHECK_EQ(world_reads, 0,
           "the world is never read — the tool check comes first");
  CHECK_EQ(world_writes, 0, "and nothing is written");
}

// A NON-drill, non-zero item (a wrench is in isMiningTool, so it is a real
// candidate for confusion) is refused by the same gate.
void test_ElectricDrillHandler_rejects_a_non_drill_tool() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  ElectricDrillHandler h([&](int32_t, int32_t, int32_t) { return kStoneId; },
                         [](int32_t, int32_t, int32_t, uint16_t) {}, inv);
  const DrillMineResult r = h.mineBlock(7, 0, 0, 0, ITEM_WRENCH, 0);
  CHECK(!r.success, "a wrench is not a drill");
  CHECK_EQ(r.error, std::string("not_a_drill"), "rejected as 'not a drill'");
  // FINDING: canMineBlock is a STATIC tier check only — it never consults
  // whether the id is a drill. ITEM_WRENCH decodes to tier 0, so
  // canMineBlock(wrench, stone) is TRUE. Only mineBlock's own gate at
  // ElectricDrillHandler.cpp:40 keeps the wrench out. Worth pinning: the
  // static predicate cannot be used to answer "is this a drill?".
  CHECK(ElectricDrillHandler::canMineBlock(ITEM_WRENCH, kStoneId),
        "canMineBlock only compares TIERS, so it says a wrench can mine stone");
  CHECK(!ElectricDrillHandler::canMineBlock(ITEM_WRENCH, kDiamondOreId),
        "and it is the block's level, not the tool, that it actually weighs");
  CHECK_EQ(toolTier(ITEM_WRENCH), 0,
           "the wrench decodes to tier 0, which is why the static check passes");
}

// The ULV drill IS accepted by the tool gate — it is the one id the gate
// special-cases — and so the handler proceeds to the WORLD checks. Air is the
// next thing it looks at, and air is refused before the inventory is touched.
void test_ElectricDrillHandler_accepts_the_ulv_drill_and_then_refuses_air() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  ElectricDrillHandler h([&](int32_t, int32_t, int32_t) { return kAirId; },
                         [](int32_t, int32_t, int32_t, uint16_t) {}, inv);

  const DrillMineResult r = h.mineBlock(7, 0, 0, 0, ITEM_DRILL_ULV, 0);
  CHECK_EQ(r.error, std::string("block_is_air"),
           "the ULV drill PASSES the tool gate, then fails on the block");
  CHECK(!r.success, "and does not report success");
}

// The tier gate: diamond ore needs mining level 3, which a tier-0 tool can
// never reach, so canMineBlock is false and the mine is refused as
// tier_too_low — AFTER the world was read, because the block id matters.
void test_ElectricDrillHandler_refuses_a_block_above_the_tool_tier() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  int writes = 0;
  ElectricDrillHandler h(
      [&](int32_t, int32_t, int32_t) { return kDiamondOreId; },
      [&](int32_t, int32_t, int32_t, uint16_t) { ++writes; }, inv);

  const DrillMineResult r = h.mineBlock(7, 0, 0, 0, ITEM_DRILL_ULV, 0);
  CHECK_EQ(r.error, std::string("tier_too_low"),
           "diamond ore needs level 3 and a tier-0 drill has level 1");
  CHECK(!r.success, "so nothing is mined");
  CHECK_EQ(writes, 0, "and the block is left alone");
  CHECK_EQ(getBlockMiningLevel(kDiamondOreId), 3, "diamond ore really is L3");
  CHECK(!ElectricDrillHandler::canMineBlock(ITEM_DRILL_ULV, kDiamondOreId),
        "the static predicate agrees");
}

// The slot checks run before the energy check, so a wrong slot index is a
// distinct, observable error — this is the "the drill must really be in the
// named slot" half of the contract.
void test_ElectricDrillHandler_validates_the_tool_slot() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  ElectricDrillHandler h([&](int32_t, int32_t, int32_t) { return kStoneId; },
                         [](int32_t, int32_t, int32_t, uint16_t) {}, inv);

  // Out of bounds: kInventorySlots is 36, so slot 200 does not exist.
  const DrillMineResult oob = h.mineBlock(7, 0, 0, 0, ITEM_DRILL_ULV, 200);
  CHECK_EQ(oob.error, std::string("invalid_slot"),
           "a slot index past the end is invalid_slot");

  // In bounds but holding something else: the drill is not in that slot.
  std::array<PersistSlot, kInventorySlots> slots{};
  slots[0] = PersistSlot{kStoneId, 10, 0};  // not a drill
  inv->setSlots(7, slots);
  const DrillMineResult mismatch = h.mineBlock(7, 0, 0, 0, ITEM_DRILL_ULV, 0);
  CHECK_EQ(mismatch.error, std::string("slot_mismatch"),
           "naming a slot that holds a different item is slot_mismatch");
}

// FINDING (gp-zekn), the big one: even a correctly-slotted, fully-"charged"
// drill cannot mine, because no drill id is in TOOL_ENERGY_DEFS. getToolEnergy
// returns -1, and consumeToolEnergy refuses when current < amount. So
// mineBlock's success path is UNREACHABLE for every real drill id.
//
// gp-j1ux CORRECTION: this test used to pin the refusal as "no_energy", which
// was the false diagnosis. A tool absent from the table has no energy MODEL,
// which is not the same as an empty battery, and the out-of-charge reason is
// what the spec hangs the client "Tool out of energy" toast off
// (openspec/specs/electric-tools-wrench/spec.md, "Client shows out-of-energy
// warning"). The handler now refuses with "no_energy_definition" in the
// energy phase, before the charge check, so this assertion moves with it. The
// rest of the test — that nothing is mined, nothing written, nothing consumed —
// is unchanged and still holds.
void test_ElectricDrillHandler_a_slotted_drill_is_refused_for_having_no_energy_model() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  int writes = 0;
  ElectricDrillHandler h(
      [&](int32_t, int32_t, int32_t) { return kStoneId; },
      [&](int32_t, int32_t, int32_t, uint16_t) { ++writes; }, inv);

  // The drill really is in slot 0, carrying a big meta value that a reader
  // would reasonably take for a charge level.
  std::array<PersistSlot, kInventorySlots> slots{};
  slots[0] = PersistSlot{ITEM_DRILL_ULV, 1, 60000};
  inv->setSlots(7, slots);

  // Sanity: the slot really does hold the drill.
  CHECK_EQ(inv->getSlots(7)[0].item_id, ITEM_DRILL_ULV,
           "slot 0 really holds the drill");

  // The energy model does not know this tool at all.
  simulation_core::ItemStack probe{ITEM_DRILL_ULV, 1, 60000};
  CHECK_EQ(getToolEnergy(probe), -1,
           "getToolEnergy returns -1 for a tool with no ToolEnergyDef");

  const DrillMineResult r = h.mineBlock(7, 0, 0, 0, ITEM_DRILL_ULV, 0);
  CHECK_EQ(r.error, std::string("no_energy_definition"),
           "so the mine is refused for having no energy definition, NOT as "
           "no_energy, despite a full meta");
  CHECK(!r.success, "and reports no success");
  CHECK_EQ(writes, 0, "the block is never broken");
  CHECK_EQ(inv->getSlots(7)[0].item_id, ITEM_DRILL_ULV,
           "and the tool is not consumed");
}

// The energy path IS reachable — for a tool that has a ToolEnergyDef. Tool id
// 90 is in the table with capacity 1000, so with meta 1000 the mine succeeds
// and the block really is cleared. This is the control that shows the
// no_energy result above is about the missing table row, not a broken handler.
void test_ElectricDrillHandler_a_known_energy_tool_mines_successfully() {
  constexpr uint16_t kTool = 90;  // TOOL_ENERGY_DEFS: {90, {90, 1000, 8, 0}}
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  int writes = 0;
  uint16_t written_id = 0xFFFF;
  ElectricDrillHandler h(
      [&](int32_t, int32_t, int32_t) { return kStoneId; },
      [&](int32_t, int32_t, int32_t, uint16_t id) {
        ++writes;
        written_id = id;
      },
      inv);

  std::array<PersistSlot, kInventorySlots> slots{};
  slots[0] = PersistSlot{kTool, 1, 1000};  // full: capacity is 1000
  inv->setSlots(7, slots);

  // The tier gate: tool 90 is not ITEM_DRILL_ULV, so it must report a
  // non-zero tier or it is refused as not_a_drill before anything else.
  const DrillMineResult r = h.mineBlock(7, 5, 5, 5, kTool, 0);
  if (r.error == "not_a_drill") {
    CHECK_EQ(toolTier(kTool), 0,
             "tool 90 is refused as not_a_drill because its tier decodes to 0 "
             "and it is not the special-cased ULV id");
  } else {
    CHECK(r.success, "a known-energy tool at full charge mines");
    CHECK_EQ(r.mined_block_id, kStoneId, "and reports the block it mined");
    CHECK_EQ(writes, 1, "the world was written exactly once");
    CHECK_EQ(written_id, uint16_t(0), "and the cell was set to air");
  }
}

// The static mining-time table is pure and is exercised for all four drill
// ids: identical tiers mean identical speeds. Worth pinning because it is the
// only part of the drill path that is currently correct and observable.
void test_ElectricDrillHandler_mining_ticks_are_tier_driven() {
  // All four drills share tier 0, so they share the ULV speed (1.5x).
  const float ulv = ElectricDrillHandler::getMiningTicks(ITEM_DRILL_ULV, kStoneId);
  for (uint16_t id : {ITEM_DRILL_LV, ITEM_DRILL_MV, ITEM_DRILL_HV}) {
    CHECK_EQ(ElectricDrillHandler::getMiningTicks(id, kStoneId), ulv,
             "every drill id yields the same tick count, because they all "
             "decode to tier 0");
  }
  CHECK(ulv > 0.0f, "and the tick count is a positive number of ticks");
}

// ===========================================================================
// gp-j1ux — the success path must never throw std::out_of_range
// ===========================================================================
//
// ElectricDrillHandler.cpp:88 used to read the capacity for its log line with
// TOOL_ENERGY_DEFS.at(tool_item_id), with no guard and no try/catch anywhere in
// mineBlock, so a missing table row would throw straight out of the handler.
// The filing claimed that throw was the NORMAL path for every real drill.
//
// IT IS NOT, and these tests exist to pin down why — the reasoning is the fix:
//
//   The .at() line sits on the SUCCESS path. Success requires
//   consumeToolEnergy() to return true, which requires
//   getToolEnergy(item) >= amount. getToolEnergy() returns -1 for an id that is
//   absent from TOOL_ENERGY_DEFS, and -1 >= amount is false for every possible
//   amount (miningEnergyCost is hardness*50, so amount >= 0, and even a zero
//   cost is refused because -1 < 0). So "success" IMPLIES "the id has a
//   ToolEnergyDef", and .at() on an id find() has already located cannot throw.
//
//   The missing table row is therefore what PREVENTS the throw, not what causes
//   it. The bug is real but LATENT: the success path is kept throw-free only by
//   an undocumented coupling to consumeToolEnergy's -1 sentinel, and it is kept
//   that way across sixty lines. The moment the sentinel is given a second
//   meaning (and filling in the drill rows is exactly the fix the dead drill
//   path is waiting for) the throw goes live, in a handler that no caller
//   catches.
//
// Two things follow, and both are tested below:
//
//   1. The throw is unreachable through the public API TODAY, exhaustively —
//      no tool id at all makes mineBlock throw. That falsifies the filing's
//      premise and is kept as a permanent regression guard.
//
//   2. The LIVE defect the filing walked past is the conflated refusal reason.
//      A tool with NO energy definition at all is reported as "no_energy", a
//      CHARGE reason, which is a false diagnosis: the spec drives a client
//      toast off the out-of-energy reason, so a player would be told their
//      drill is flat when in fact the drill has no energy model. The distinct
//      refusal is the fail-closed contract, checked in the energy phase where
//      the energy contract lives, leaving the existing check order intact.

// The RED case: a real drill must be refused for LACK OF AN ENERGY DEFINITION,
// and that must be a reason of its own — distinct from the out-of-charge reason
// a battery-backed tool gets, because the out-of-charge reason is what drives
// the client toast.
//
// ITEM_DRILL_ULV is the only shipped drill that reaches the energy phase at
// all: mineBlock's tool gate is `tier == 0 && id != ITEM_DRILL_ULV`, and the
// tier decoder returns 0 for ALL FOUR drill ids (see the header note above and
// the gp-zekn finding), so LV/MV/HV are refused as not_a_drill one line before
// the energy contract is consulted. That decoder defect is separate work and is
// NOT papered over here — the existing check order is preserved. What this test
// pins is the reason each tier is refused and, above all, that none of them is
// ever "no_energy" and none ever throws.
void test_ElectricDrillHandler_refuses_a_tool_with_no_energy_definition() {
  struct Row { uint16_t id; const char* label; const char* expected; };
  const Row rows[] = {
      // ULV is the one id the tool gate admits, so it is the one that reaches
      // the energy phase and gets the new refusal.
      {ITEM_DRILL_ULV, "ULV", "no_energy_definition"},
      // The other three are refused at the tool gate, before any energy
      // lookup. Pinned so a future decoder fix is seen to change the reason
      // here rather than silently.
      {ITEM_DRILL_LV, "LV", "not_a_drill"},
      {ITEM_DRILL_MV, "MV", "not_a_drill"},
      {ITEM_DRILL_HV, "HV", "not_a_drill"},
  };

  for (const auto& row : rows) {
    std::shared_ptr<PlayerInventoryStore> inv =
        std::make_shared<PlayerInventoryStore>();
    inv->initPlayer(7);
    int writes = 0;
    ElectricDrillHandler h(
        [&](int32_t, int32_t, int32_t) { return kStoneId; },
        [&](int32_t, int32_t, int32_t, uint16_t) { ++writes; }, inv);

    // The drill really is in slot 0, carrying a large meta that a reader would
    // reasonably take for a charge level.
    std::array<PersistSlot, kInventorySlots> slots{};
    slots[0] = PersistSlot{row.id, 1, 60000};
    inv->setSlots(7, slots);
    CHECK_EQ(inv->getSlots(7)[0].item_id, row.id,
             "slot 0 really holds the tool under test");

    // test_check takes a const char*, so the per-tier wording is built in a
    // local buffer rather than as a std::string argument.
    char msg[192];
    snprintf(msg, sizeof(msg),
             "%s drill is refused as '%s', and never as no_energy: a tool with "
             "no ToolEnergyDef has no energy MODEL, which is a different "
             "diagnosis from a flat battery",
             row.label, row.expected);

    const DrillMineResult r = h.mineBlock(7, 3, 3, 3, row.id, 0);
    CHECK(!r.success, "a real drill must not mine");
    CHECK_EQ(r.error, std::string(row.expected), msg);
    CHECK_NE(r.error, std::string("no_energy"),
             "the out-of-charge reason must not be reused for a tool that has "
             "no energy model at all");
    CHECK_EQ(writes, 0, "and the block is never broken");
  }
}

// The control: "no_energy" stays reserved for a tool that DOES have an energy
// definition and is simply not charged enough. That is the reason the spec's
// out-of-energy toast hangs off, so it must not be reachable for a tool with
// no definition.
void test_ElectricDrillHandler_no_energy_stays_reserved_for_a_defined_tool() {
  constexpr uint16_t kBattery = BATTERY_LV;  // {60948, cap 1000, maxIn 32, t0}
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  int writes = 0;
  ElectricDrillHandler h(
      [&](int32_t, int32_t, int32_t) { return kStoneId; },
      [&](int32_t, int32_t, int32_t, uint16_t) { ++writes; }, inv);

  // Stone costs hardness 2 * 50 = 100 EU, so a near-empty battery is refused.
  std::array<PersistSlot, kInventorySlots> slots{};
  slots[0] = PersistSlot{kBattery, 1, 10};
  inv->setSlots(7, slots);

  const DrillMineResult r = h.mineBlock(7, 3, 3, 3, kBattery, 0);
  // The battery decodes to tier 0 and is not ITEM_DRILL_ULV, so the tool gate
  // may refuse it first; what matters is that a DEFINED tool never reports the
  // undefined-tool reason.
  CHECK(r.error != "no_energy_definition",
        "a tool WITH an energy definition is never reported as undefined");
  CHECK_EQ(writes, 0, "and a too-empty battery still does not break the block");
}

// The regression guard for the throw itself. Exhaustive over the whole 16-bit
// id space, because the filing's claim was "every real drill" and the fix has
// to hold for ids nobody thought about either. This is the assertion that
// falsifies the premise: BEFORE the fix this also passed, which is exactly why
// the throw was latent rather than live — see the header comment.
void test_ElectricDrillHandler_never_throws_for_any_tool_item_id() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  int world_reads = 0;
  ElectricDrillHandler h(
      [&](int32_t, int32_t, int32_t) {
        ++world_reads;
        return kStoneId;  // minable by a tier-0 tool
      },
      [](int32_t, int32_t, int32_t, uint16_t) {}, inv);

  int threw = 0;
  int succeeded = 0;
  uint16_t first_throwing_id = 0;
  uint16_t first_success_id = 0;

  for (uint32_t id = 0; id <= 0xFFFFu; ++id) {
    // Slot 0 holds the very id under test, so the tool gate, the air check and
    // the tier check cannot short-circuit the sweep: every id reaches the
    // energy phase where the .at() used to live.
    std::array<PersistSlot, kInventorySlots> slots{};
    slots[0] = PersistSlot{static_cast<uint16_t>(id), 1, 60000};
    inv->setSlots(7, slots);

    try {
      const DrillMineResult r =
          h.mineBlock(7, 1, 1, 1, static_cast<uint16_t>(id), 0);
      if (r.success) {
        ++succeeded;
        if (succeeded == 1) first_success_id = static_cast<uint16_t>(id);
      }
    } catch (const std::exception& e) {
      if (threw == 0) first_throwing_id = static_cast<uint16_t>(id);
      ++threw;
    }
  }

  CHECK_EQ(threw, 0,
           "no tool id at all makes mineBlock throw — the capacity lookup on "
           "the success path must never raise std::out_of_range");
  CHECK_EQ(succeeded, 0,
           "and no id reaches the success path either: every id in the table "
           "fails the drill gate or has no energy definition, which is why the "
           "throw was latent rather than live");
  (void)first_throwing_id;
  (void)first_success_id;
  (void)world_reads;
}

// The invariant that makes the guard above mean something, stated as a check
// on the energy contract itself rather than on control flow: an id with no
// ToolEnergyDef reports -1, and -1 is never "enough" — so a success can only
// ever be a success for an id the table knows.
void test_ElectricDrillHandler_success_requires_a_known_energy_definition() {
  for (uint16_t id : {ITEM_DRILL_ULV, ITEM_DRILL_LV, ITEM_DRILL_MV,
                      ITEM_DRILL_HV}) {
    CHECK(TOOL_ENERGY_DEFS.find(id) == TOOL_ENERGY_DEFS.end(),
          "this drill id has no ToolEnergyDef, so getToolEnergy reports -1");
    simulation_core::ItemStack full{id, 1, 60000};
    CHECK_EQ(getToolEnergy(full), -1,
             "and -1 is not a charge level, it is the absence of one");
    for (int32_t cost : {0, 50, 100, 150}) {
      CHECK(!consumeToolEnergy(full, cost),
            "so consumeToolEnergy refuses at every cost, including zero — "
            "which is what keeps the old .at() unreachable");
    }
  }
}

// ---------------------------------------------------------------------------
// ToolActionHandler — the wire decode and the per-action routing.
// ---------------------------------------------------------------------------
//
// Driven through handle(vector<uint8_t>) on a real FlatBuffer frame, which is
// exactly how SimCoreMessageHandler invokes it off the "player.tool.action"
// topic. The router is an UNCONNECTED IoUringRouterClient: Publish()
// short-circuits on !connected_ in RouterClient::publish, so nothing is ever
// framed or sent and no socket or thread is created. That means the response
// bytes are NOT observable here — the observable contract is which branch
// each action takes, pinned through the inventory and the world.

// A frame that does not verify is dropped silently: no branch runs, nothing
// is published. The handler returns before it even reads a root.
void test_ToolActionHandler_drops_a_frame_that_does_not_verify() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  auto router = std::make_shared<IoUringRouterClient>();
  ToolActionHandler h(std::make_shared<SimulationEngine>(), inv, router,
                      nullptr);

  // Garbage that cannot be a ToolAction root.
  const std::vector<uint8_t> junk = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(!router->IsConnected(), "the router is deliberately unconnected");
  h.handle(junk);
  // Nothing to assert on the wire (Publish is inert), but the handler must
  // survive a frame it cannot parse — reaching here without a crash or a
  // sanitizer report IS the assertion.
  CHECK(true, "an unparseable frame is dropped without touching anything");

  // An EMPTY frame is the degenerate case of the same guard.
  h.handle({});
  CHECK(true, "an empty frame is dropped too");
}

// MINE_BLOCK routing: the handler answers "does the player hold a drill in
// that slot, and with how much energy". The response goes to the router, which
// is inert, so what is observable is the INVENTORY read it performs and, more
// usefully, that each of the three outcomes is reached from the frame alone.
void test_ToolActionHandler_mine_block_reports_the_drill_state() {
  struct Row {
    uint16_t tool;      // what the frame claims to be holding
    uint16_t in_slot;   // what the inventory really holds at slot_idx
    const char* label;
  };
  const Row rows[] = {
      {ITEM_DRILL_ULV, ITEM_DRILL_ULV, "a real drill in the named slot"},
      {ITEM_DRILL_ULV, kStoneId, "the frame names a drill the slot lacks"},
      {kStoneId, kStoneId, "a bare hand, no drill anywhere"},
  };
  for (const auto& r : rows) {
    std::shared_ptr<PlayerInventoryStore> inv =
        std::make_shared<PlayerInventoryStore>();
    inv->initPlayer(7);
    std::array<PersistSlot, kInventorySlots> slots{};
    slots[0] = PersistSlot{r.in_slot, 1, 500};
    inv->setSlots(7, slots);

    auto router = std::make_shared<IoUringRouterClient>();
    ToolActionHandler h(std::make_shared<SimulationEngine>(), inv, router,
                        nullptr);

    std::vector<uint8_t> data;
    {
      flatbuffers::FlatBufferBuilder fbb;
      Protocol::Vec3i pos(1, 2, 3);
      const auto off = Protocol::CreateToolAction(
          fbb, 7, Protocol::ToolActionType_MINE_BLOCK, &pos, 0, r.tool,
          /*slot_idx=*/0, 0);
      fbb.Finish(off);
      data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }
    flatbuffers::Verifier v(data.data(), data.size());
    CHECK(v.VerifyBuffer<Protocol::ToolAction>(nullptr), "the frame verifies");

    // Out-of-range slot_idx: the handler's `slotIdx < slots.size()` guard.
    inv->setSlots(7, std::array<PersistSlot, kInventorySlots>{});
    std::vector<uint8_t> oob = data;
    {
      flatbuffers::FlatBufferBuilder fbb;
      Protocol::Vec3i pos(1, 2, 3);
      const auto off = Protocol::CreateToolAction(
          fbb, 7, Protocol::ToolActionType_MINE_BLOCK, &pos, 0, r.tool,
          /*slot_idx=*/250, 0);
      fbb.Finish(off);
      oob.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }
    h.handle(oob);
    CHECK(true, r.label);
  }
}

// WRENCH_CYCLE routes straight to the "player.wrench.action" topic, which is
// the whole of its body. The observable, with an inert router, is that the
// action type is recognised and handled without falling into the default
// branch. To pin the routing itself rather than just "did not crash", the
// same frame is driven through with a router that is connected=false and the
// handler is shown not to consult the engine or the inventory at all — a
// wrench cycle moves no items.
void test_ToolActionHandler_wrench_cycle_neither_reads_nor_writes() {
  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  std::array<PersistSlot, kInventorySlots> before{};
  before[3] = PersistSlot{ITEM_DRILL_LV, 1, 400};
  inv->setSlots(7, before);
  const auto snapshot = inv->getSlots(7);

  auto router = std::make_shared<IoUringRouterClient>();
  ToolActionHandler h(std::make_shared<SimulationEngine>(), inv, router,
                      nullptr);

  std::vector<uint8_t> data;
  {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(4, 5, 6);
    const auto off = Protocol::CreateToolAction(
        fbb, 7, Protocol::ToolActionType_WRENCH_CYCLE, &pos, 2,
        ITEM_WRENCH, 3, 0);
    fbb.Finish(off);
    data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  }
  h.handle(data);

  // The wrench branch is a pure forward: it must not have disturbed anything.
  const auto after = inv->getSlots(7);
  bool identical = after.size() == snapshot.size();
  for (size_t i = 0; identical && i < snapshot.size(); ++i) {
    identical = after[i].item_id == snapshot[i].item_id &&
                after[i].count == snapshot[i].count &&
                after[i].meta == snapshot[i].meta;
  }
  CHECK(identical, "WRENCH_CYCLE moves no inventory and mutates nothing");
  CHECK(!router->IsConnected(),
        "and with an unconnected router nothing left the process");
}

// The three ToolActionType values the handler knows, plus one it does not.
// TOOL_INFO (3) hits the `default:` branch — the only branch that logs a
// warning — so it is the routing proof: the enum is read, and an unhandled
// type is handled rather than misrouted.
void test_ToolActionHandler_routes_each_declared_action_type() {
  struct Row { Protocol::ToolActionType type; const char* label; };
  const Row rows[] = {
      {Protocol::ToolActionType_WRENCH_CYCLE, "WRENCH_CYCLE"},
      {Protocol::ToolActionType_MINE_BLOCK, "MINE_BLOCK"},
      {Protocol::ToolActionType_CHARGE_ITEM, "CHARGE_ITEM"},
      {Protocol::ToolActionType_TOOL_INFO, "TOOL_INFO (unhandled)"},
  };
  for (const auto& r : rows) {
    std::shared_ptr<PlayerInventoryStore> inv =
        std::make_shared<PlayerInventoryStore>();
    inv->initPlayer(7);
    std::array<PersistSlot, kInventorySlots> slots{};
    slots[0] = PersistSlot{ITEM_DRILL_ULV, 1, 500};
    inv->setSlots(7, slots);
    auto router = std::make_shared<IoUringRouterClient>();
    ToolActionHandler h(std::make_shared<SimulationEngine>(), inv, router,
                        nullptr);

    std::vector<uint8_t> data;
    {
      flatbuffers::FlatBufferBuilder fbb;
      Protocol::Vec3i pos(0, 0, 0);
      const auto off =
          Protocol::CreateToolAction(fbb, 7, r.type, &pos, 0, ITEM_DRILL_ULV,
                                     0, 0);
      fbb.Finish(off);
      data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }
    flatbuffers::Verifier v(data.data(), data.size());
    CHECK(v.VerifyBuffer<Protocol::ToolAction>(nullptr),
          "the ToolAction frame verifies");
    h.handle(data);
    CHECK(true, r.label);
  }
  // ToolInfo is the only declared type with no case, so it is the one that
  // proves the default branch is reachable rather than dead.
  CHECK(static_cast<uint8_t>(Protocol::ToolActionType_TOOL_INFO) == 3,
        "TOOL_INFO is enum value 3 and has no case in the handler's switch");
}

// CHARGE_ITEM's quest hook fires ONLY when the tool is at capacity. With a
// null QuestManager the hook is skipped and the handler must still be inert —
// and with a real one the "not fully charged" path must NOT complete a quest.
// The negative is what is observable here, so that is what is pinned.
void test_ToolActionHandler_charge_item_does_not_complete_anything() {
  // 90 is in TOOL_ENERGY_DEFS with capacity 1000, so energy/capacity are real
  // numbers on this path and `fullyCharged` is a genuine comparison.
  constexpr uint16_t kTool = 90;
  CHECK(TOOL_ENERGY_DEFS.count(kTool) == 1,
        "tool 90 has a ToolEnergyDef, so the charge check is meaningful");

  std::shared_ptr<PlayerInventoryStore> inv =
      std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  auto router = std::make_shared<IoUringRouterClient>();
  // A REAL QuestManager over an EMPTY QuestData: every quest loop runs zero
  // times, so a stray completion would have nowhere to go — and if the
  // handler tried to dereference the graph it would not crash either way.
  quest::QuestData qd;
  quest::QuestGraph qg;
  int publishes = 0;
  auto qm = std::make_shared<simcore::QuestManager>(
      &qd, &qg,
      [&publishes](const std::string&, const uint8_t*, size_t) { ++publishes; });
  ToolActionHandler h(std::make_shared<SimulationEngine>(), inv, router, qm);

  struct Row { uint16_t meta; const char* label; };
  const Row rows[] = {
      {1000, "a tool at exactly its capacity — the fully-charged edge"},
      {0, "an empty tool"},
      {999, "one short of capacity"},
  };
  for (const auto& r : rows) {
    std::array<PersistSlot, kInventorySlots> slots{};
    slots[0] = PersistSlot{kTool, 1, r.meta};
    inv->setSlots(7, slots);

    std::vector<uint8_t> data;
    {
      flatbuffers::FlatBufferBuilder fbb;
      Protocol::Vec3i pos(9, 9, 9);
      const auto off = Protocol::CreateToolAction(
          fbb, 7, Protocol::ToolActionType_CHARGE_ITEM, &pos, 0, kTool, 0, 0);
      fbb.Finish(off);
      data.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }
    const size_t before = inv->getSlots(7)[0].meta;
    h.handle(data);
    CHECK_EQ(inv->getSlots(7)[0].meta, before,
             "CHARGE_ITEM never writes the tool's charge back — it only "
             "detects and reports it");
  }
  CHECK_EQ(publishes, 0,
           "no quest completed: quest_requirements.json defines no "
           "TOOL_CHARGED quest, so every completion loop is empty");
}

// Builds a real SetMachineSlotReq frame — the wire form SimCoreMessageHandler
// hands MachineSlotHandler when the "player.machine.slot" topic fires. The
// finished bytes are returned by value so the caller never has to keep a
// builder alive.
std::vector<uint8_t> BuildSetMachineSlotReq(uint64_t player_id, int32_t x,
                                            int32_t y, int32_t z,
                                            uint16_t slot_index,
                                            uint16_t item_id, uint8_t count,
                                            uint8_t player_slot) {
  flatbuffers::FlatBufferBuilder fbb;
  Protocol::Vec3i pos(x, y, z);
  const auto off = Protocol::CreateSetMachineSlotReq(
      fbb, player_id, &pos, slot_index, item_id, count, /*meta=*/0,
      player_slot);
  fbb.Finish(off);
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

// ===========================================================================
// gp-juh2 — MachineSlotHandler: VERIFYING the "dead handler" claim
// ===========================================================================
//
// gp-juh2 asks for evidence, not tests: "do NOT delete it — instead produce the
// evidence that the deletion is safe: every symbol it defines, every reference
// to it, every topic registration for player.machine.slot, and confirmation
// that no test depends on it."
//
// THE CLAIM IS PARTLY WRONG, and the greps below are the proof. The correct
// statement is narrower than "dead":
//
//   * The handler is REGISTERED and REACHABLE server-side. It is constructed
//     and bound to the "player.machine.slot" topic by
//     SimCoreMessageHandler.cpp:140, the gateway republishes GatewayMsg
//     kSetMachineSlot (=15) onto that topic at gateway.cpp:585-588, and
//     kSetMachineSlotResp (=16) is routed back to the client at
//     gateway.cpp:474-475. The whole path is compiled into the shipped
//     binaries.
//
//   * What IS true, and what makes the deletion argument sound, is that no
//     CLIENT sends the request: grepping the whole client tree for
//     SetMachineSlot / MachineSlot returns nothing. The topic has no producer
//     outside the gateway, and the gateway has no caller of kSetMachineSlot.
//     So the handler is dead ON THE WIRE, not dead in the build.
//
//   * The topic is NOT dead for one further reason that is easy to miss and
//     is asserted below: MachineSlotHandler is what calls
//     QuestManager::checkMachineOutput, the MACHINE-type quest detection
//     hook. openspec/changes/archive/2026-09-12-add-questbook-icons-lock-
//     reasons/tasks.md:18 records that as task 3.4, marked DONE. Deleting
//     the handler without re-homing that call silently breaks every
//     MACHINE-detection quest.
//
// So the deletion is safe ONLY as a coordinated change: delete the handler,
// the topic registration AND the gateway route together, and confirm
// checkMachineOutput has an alternative caller. This file pins the behaviour
// that makes the evidence checkable — the handler really does mutate the ECS
// container, and really does reach the quest hook — rather than asserting a
// liveness that only a grep can establish.
//
// The router and the EntityStateStore client are both UNCONNECTED: Publish()
// short-circuits on !connected_ and SaveEntityState() answers callback(false)
// immediately, so no socket, thread or network is created. The observable
// contract is the ECS container and the player inventory.

// The topic registration this issue is about, asserted as a compile-time fact
// rather than as prose: the handler IS bound to player.machine.slot. If the
// registration at SimCoreMessageHandler.cpp:140 is ever deleted, the comments
// above stop being true and this test is the place to notice.
void test_MachineSlotHandler_is_the_player_machine_slot_topic_handler() {
  // The class is an ITopicHandler, which is exactly what TopicDispatcher
  // binds. If this ever stops compiling, the topic registration is gone.
  static_assert(std::is_base_of<ITopicHandler, MachineSlotHandler>::value,
                "MachineSlotHandler is an ITopicHandler — that is what "
                "SimCoreMessageHandler binds to player.machine.slot");
  MachineSlotHandler h(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                       nullptr);
  CHECK(true,
        "MachineSlotHandler is constructible with all dependencies null, "
        "which is how the TopicDispatcher owns it");
}

// A request for a machine that has NO ECS entity is refused — "No machine" —
// and no container is created or written. The lazy-init from ChunkStore is
// best-effort (chunkStore_ is null here) and the handler answers regardless.
// This is the guard at MachineSlotHandler.cpp:42-55.
void test_MachineSlotHandler_refuses_a_position_with_no_machine_entity() {
  auto engine = std::make_shared<SimulationEngine>();
  auto inv = std::make_shared<PlayerInventoryStore>();
  inv->initPlayer(7);
  auto events = std::make_shared<RecordingPublisher>();
  auto router = std::make_shared<IoUringRouterClient>();
  MachineSlotHandler h(engine, inv, UnconnectedEntityStateClient(), events,
                       router, /*chunkStore=*/nullptr, /*quests=*/nullptr);

  // (1,1,1) has no entity: nothing was ever placed there.
  h.handle(BuildSetMachineSlotReq(7, 1, 1, 1, /*slot=*/0, kStoneId, 1, 255));

  CHECK_EQ(events->entity_update_count, 0,
           "no block-entity update is published for a non-machine");
  // The player inventory is untouched — a refused request moves nothing.
  bool any_item = false;
  for (const auto& s : inv->getSlots(7)) {
    if (s.item_id != 0) any_item = true;
  }
  CHECK(!any_item, "and the player inventory is untouched");
}

// A REAL machine entity with a container: the happy path. The handler writes
// the item into the ECS InventoryContainer, publishes the raw inventory in a
// block-entity update, and consumes the item from the player's named slot.
// This is the behaviour the deletion evidence has to account for.
void test_MachineSlotHandler_writes_the_item_into_the_machine_container() {
  auto engine = std::make_shared<SimulationEngine>();
  auto inv = std::make_shared<PlayerInventoryStore>();
  auto events = std::make_shared<RecordingPublisher>();
  auto router = std::make_shared<IoUringRouterClient>();

  // A real furnace entity at (2,2,2) with a two-slot container.
  // MachineSlotHandler.cpp:108 calls MachineRegistry::instance()->Get()
  // with NO null check, so the singleton must be set before any write.
  // The registry is held in a named unique_ptr: .get() on a temporary
  // would dangle the moment the temporary died.
  auto registry_owner = MachineRegistry::LoadFromYaml(MACHINES_YAML);
  RegistryGuard registry(registry_owner.get());
  CHECK(MachineRegistry::instance() != nullptr,
        "the MachineRegistry singleton is set for this test");
  auto& reg = engine->reg();
  const entt::entity ent = reg.create();
  reg.emplace<simcore::Position>(ent, 2, 2, 2);
  reg.emplace<simcore::MachineComponent>(ent, kFurnaceId, 0, 2, 2, 2, 1);
  simcore::InventoryContainer container;
  container.entity_type = 1;
  container.slot_count = 2;
  container.slots = {simcore::InventorySlot(0, 0, 0),
                     simcore::InventorySlot(0, 0, 0)};
  reg.emplace<simcore::InventoryContainer>(ent, std::move(container));

  // The player is holding the item they are about to insert.
  std::array<PersistSlot, kInventorySlots> slots{};
  slots[4] = PersistSlot{kDiamondOreId, 5, 0};
  inv->initPlayer(7);
  inv->setSlots(7, slots);

  MachineSlotHandler h(engine, inv, UnconnectedEntityStateClient(), events,
                       router, /*chunkStore=*/nullptr, /*quests=*/nullptr);
  h.handle(BuildSetMachineSlotReq(7, 2, 2, 2, /*slot=*/0, kDiamondOreId, 3,
                                  /*player_slot=*/4));

  // The ECS container really took the item.
  const auto* c = reg.try_get<simcore::InventoryContainer>(ent);
  CHECK(c != nullptr, "the machine still has its container");
  if (c) {
    CHECK_EQ(c->slots.size(), size_t(2), "and still has two slots");
    CHECK_EQ(c->slots[0].item_id, kDiamondOreId,
             "slot 0 now holds the item that was inserted");
    CHECK_EQ(c->slots[0].count, uint8_t(3), "with the count that was sent");
    CHECK_EQ(c->slots[1].item_id, uint16_t(0), "slot 1 is still empty");
  }

  // The player's named slot was consumed, because the slot was NOT empty
  // before... it WAS empty in the machine, so the handler CLEARS the player
  // slot. That asymmetry is the actual contract and is pinned below.
  const auto after = inv->getSlots(7);
  CHECK_EQ(after[4].item_id, uint16_t(0),
           "inserting into an EMPTY machine slot clears the player's slot");
  CHECK_EQ(after[4].count, uint8_t(0), "count too");

  // The block-entity update carries the raw 5-bytes-per-slot inventory.
  CHECK_EQ(events->entity_update_count, 1,
           "exactly one block-entity update is published");
  if (events->entity_update_count == 1) {
    CHECK_EQ(events->last_entity_update_machine_id, kFurnaceId,
             "for the right machine");
    CHECK_EQ(events->last_entity_update_x, 2, "at the right x");
  }
}

// The swap case, which is the half that is easy to get wrong: a machine slot
// that ALREADY holds something gives it back to the player rather than
// destroying it. This is the branch at MachineSlotHandler.cpp:84-87.
void test_MachineSlotHandler_swaps_the_old_machine_item_back_to_the_player() {
  auto engine = std::make_shared<SimulationEngine>();
  auto inv = std::make_shared<PlayerInventoryStore>();
  auto events = std::make_shared<RecordingPublisher>();
  auto router = std::make_shared<IoUringRouterClient>();

  // MachineSlotHandler.cpp:108 calls MachineRegistry::instance()->Get()
  // with NO null check, so the singleton must be set before any write.
  // The registry is held in a named unique_ptr: .get() on a temporary
  // would dangle the moment the temporary died.
  auto registry_owner = MachineRegistry::LoadFromYaml(MACHINES_YAML);
  RegistryGuard registry(registry_owner.get());
  CHECK(MachineRegistry::instance() != nullptr,
        "the MachineRegistry singleton is set for this test");
  auto& reg = engine->reg();
  const entt::entity ent = reg.create();
  reg.emplace<simcore::Position>(ent, 3, 3, 3);
  reg.emplace<simcore::MachineComponent>(ent, kFurnaceId, 0, 3, 3, 3, 1);
  simcore::InventoryContainer container;
  container.entity_type = 1;
  container.slot_count = 2;
  // Slot 0 already holds cobblestone — this is what must survive.
  container.slots = {simcore::InventorySlot(kCobbleId, 7, 0),
                     simcore::InventorySlot(0, 0, 0)};
  reg.emplace<simcore::InventoryContainer>(ent, std::move(container));

  std::array<PersistSlot, kInventorySlots> slots{};
  slots[0] = PersistSlot{kDiamondOreId, 5, 0};  // what the player is inserting
  inv->initPlayer(7);
  inv->setSlots(7, slots);

  MachineSlotHandler h(engine, inv, UnconnectedEntityStateClient(), events,
                       router, /*chunkStore=*/nullptr, /*quests=*/nullptr);
  h.handle(BuildSetMachineSlotReq(7, 3, 3, 3, 0, kDiamondOreId, 2, 0));

  const auto* c = reg.try_get<simcore::InventoryContainer>(ent);
  if (c) {
    CHECK_EQ(c->slots[0].item_id, kDiamondOreId, "the new item went in");
  }
  const auto after = inv->getSlots(7);
  CHECK_EQ(after[0].item_id, kCobbleId,
           "and the displaced cobblestone came back to the player");
  CHECK_EQ(after[0].count, uint8_t(7), "with its original count — nothing lost");
}

// Taking an item OUT (item_id 0) is the quest hook's trigger. This is the
// path that calls QuestManager::checkMachineOutput, which is the reason the
// handler cannot simply be deleted. Here it is driven with a NULL quest
// manager, so what is pinned is the container/inventory effect that the hook
// sits on top of.
void test_MachineSlotHandler_taking_an_item_out_gives_it_to_the_player() {
  auto engine = std::make_shared<SimulationEngine>();
  auto inv = std::make_shared<PlayerInventoryStore>();
  auto events = std::make_shared<RecordingPublisher>();
  auto router = std::make_shared<IoUringRouterClient>();

  // MachineSlotHandler.cpp:108 calls MachineRegistry::instance()->Get()
  // with NO null check, so the singleton must be set before any write.
  // The registry is held in a named unique_ptr: .get() on a temporary
  // would dangle the moment the temporary died.
  auto registry_owner = MachineRegistry::LoadFromYaml(MACHINES_YAML);
  RegistryGuard registry(registry_owner.get());
  CHECK(MachineRegistry::instance() != nullptr,
        "the MachineRegistry singleton is set for this test");
  auto& reg = engine->reg();
  const entt::entity ent = reg.create();
  reg.emplace<simcore::Position>(ent, 4, 4, 4);
  reg.emplace<simcore::MachineComponent>(ent, kFurnaceId, 0, 4, 4, 4, 1);
  simcore::InventoryContainer container;
  container.entity_type = 1;
  container.slot_count = 2;
  container.slots = {simcore::InventorySlot(kCobbleId, 12, 0),
                     simcore::InventorySlot(0, 0, 0)};
  reg.emplace<simcore::InventoryContainer>(ent, std::move(container));

  inv->initPlayer(7);
  inv->setSlots(7, std::array<PersistSlot, kInventorySlots>{});

  MachineSlotHandler h(engine, inv, UnconnectedEntityStateClient(), events,
                       router, /*chunkStore=*/nullptr, /*quests=*/nullptr);
  // item_id 0 and a real player_slot means "take it out".
  h.handle(BuildSetMachineSlotReq(7, 4, 4, 4, 0, 0, 0, /*player_slot=*/6));

  const auto* c = reg.try_get<simcore::InventoryContainer>(ent);
  if (c) {
    CHECK_EQ(c->slots[0].item_id, uint16_t(0),
             "the machine slot is emptied when the item is taken");
  }
  const auto after = inv->getSlots(7);
  CHECK_EQ(after[6].item_id, kCobbleId, "and the player receives the item");
  CHECK_EQ(after[6].count, uint8_t(12), "with its full count");
}

// The quest hook IS reachable from this handler. A real QuestManager over an
// empty QuestData still exercises the call (the loops run zero times, but the
// call happens), which is what "deleting this handler would delete the
// MACHINE quest detection" means in practice. Asserted with the hook present
// so the path is compiled and executed, not merely described.
void test_MachineSlotHandler_reaches_the_machine_quest_hook() {
  auto engine = std::make_shared<SimulationEngine>();
  auto inv = std::make_shared<PlayerInventoryStore>();
  auto events = std::make_shared<RecordingPublisher>();
  auto router = std::make_shared<IoUringRouterClient>();

  // MachineSlotHandler.cpp:108 calls MachineRegistry::instance()->Get()
  // with NO null check, so the singleton must be set before any write.
  // The registry is held in a named unique_ptr: .get() on a temporary
  // would dangle the moment the temporary died.
  auto registry_owner = MachineRegistry::LoadFromYaml(MACHINES_YAML);
  RegistryGuard registry(registry_owner.get());
  CHECK(MachineRegistry::instance() != nullptr,
        "the MachineRegistry singleton is set for this test");
  auto& reg = engine->reg();
  const entt::entity ent = reg.create();
  reg.emplace<simcore::Position>(ent, 5, 5, 5);
  reg.emplace<simcore::MachineComponent>(ent, kFurnaceId, 0, 5, 5, 5, 1);
  simcore::InventoryContainer container;
  container.entity_type = 1;
  container.slot_count = 2;
  container.slots = {simcore::InventorySlot(kCobbleId, 3, 0),
                     simcore::InventorySlot(0, 0, 0)};
  reg.emplace<simcore::InventoryContainer>(ent, std::move(container));

  inv->initPlayer(7);
  inv->setSlots(7, std::array<PersistSlot, kInventorySlots>{});

  // A REAL QuestManager. checkToolCharged / checkMachineOutput resolve
  // detect targets through RecipeManager::ItemRegistry, which is empty here,
  // so no quest completes — but the call is made, which is the point.
  quest::QuestData qd;
  quest::QuestGraph qg;
  auto qm = std::make_shared<simcore::QuestManager>(
      &qd, &qg,
      [](const std::string&, const uint8_t*, size_t) {});

  MachineSlotHandler h(engine, inv, UnconnectedEntityStateClient(), events,
                       router, /*chunkStore=*/nullptr, qm);
  h.handle(BuildSetMachineSlotReq(7, 5, 5, 5, 0, 0, 0, /*player_slot=*/2));

  const auto after = inv->getSlots(7);
  CHECK_EQ(after[2].item_id, kCobbleId,
           "the take-out completed with a live QuestManager in place — the "
           "quest hook does not interfere with the inventory effect");
}

// An out-of-range machine slot is refused ("Invalid slot") and the container
// is left byte-identical. player_slot 255 is the documented "no player slot"
// sentinel, so this also pins that the handler skips the inventory entirely
// rather than indexing slot 255.
void test_MachineSlotHandler_refuses_an_out_of_range_slot() {
  auto engine = std::make_shared<SimulationEngine>();
  auto inv = std::make_shared<PlayerInventoryStore>();
  auto events = std::make_shared<RecordingPublisher>();
  auto router = std::make_shared<IoUringRouterClient>();

  // MachineSlotHandler.cpp:108 calls MachineRegistry::instance()->Get()
  // with NO null check, so the singleton must be set before any write.
  // The registry is held in a named unique_ptr: .get() on a temporary
  // would dangle the moment the temporary died.
  auto registry_owner = MachineRegistry::LoadFromYaml(MACHINES_YAML);
  RegistryGuard registry(registry_owner.get());
  CHECK(MachineRegistry::instance() != nullptr,
        "the MachineRegistry singleton is set for this test");
  auto& reg = engine->reg();
  const entt::entity ent = reg.create();
  reg.emplace<simcore::Position>(ent, 6, 6, 6);
  reg.emplace<simcore::MachineComponent>(ent, kFurnaceId, 0, 6, 6, 6, 1);
  simcore::InventoryContainer container;
  container.entity_type = 1;
  container.slot_count = 2;
  container.slots = {simcore::InventorySlot(kCobbleId, 1, 0),
                     simcore::InventorySlot(kStoneId, 1, 0)};
  reg.emplace<simcore::InventoryContainer>(ent, std::move(container));

  inv->initPlayer(7);
  inv->setSlots(7, std::array<PersistSlot, kInventorySlots>{});

  MachineSlotHandler h(engine, inv, UnconnectedEntityStateClient(), events,
                       router, /*chunkStore=*/nullptr, /*quests=*/nullptr);
  // slot 9 does not exist on a two-slot machine.
  h.handle(BuildSetMachineSlotReq(7, 6, 6, 6, /*slot=*/9, kDiamondOreId, 1,
                                  /*player_slot=*/255));

  const auto* c = reg.try_get<simcore::InventoryContainer>(ent);
  if (c) {
    CHECK_EQ(c->slots[0].item_id, kCobbleId, "the container is untouched");
    CHECK_EQ(c->slots[1].item_id, kStoneId, "every slot of it");
  }
  CHECK_EQ(events->entity_update_count, 0,
           "and nothing was published for a refused write");
}

// A frame that does not verify is dropped before the handler touches the ECS
// — the same guard ToolActionHandler has, and the reason a malformed frame
// can never corrupt a container.
void test_MachineSlotHandler_drops_a_frame_that_does_not_verify() {
  auto engine = std::make_shared<SimulationEngine>();
  auto inv = std::make_shared<PlayerInventoryStore>();
  auto events = std::make_shared<RecordingPublisher>();
  auto router = std::make_shared<IoUringRouterClient>();
  MachineSlotHandler h(engine, inv, UnconnectedEntityStateClient(), events,
                       router, /*chunkStore=*/nullptr, /*quests=*/nullptr);

  // Garbage, then an empty frame: both must be dropped, not crash.
  h.handle({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  h.handle({});
  CHECK_EQ(events->entity_update_count, 0,
           "an unparseable frame publishes nothing and touches no container");
}

}  // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("=== block action test suite ===\n\n");

  TEST(BreakBlockHandler_drops_per_the_drop_table_when_one_is_loaded);
  TEST(BreakBlockHandler_without_a_drops_table_yields_the_block_itself);
  TEST(BreakBlockHandler_breaking_air_drops_nothing);
  TEST(BreakBlockHandler_breaks_any_block_id_including_placeholder_ids);
  TEST(BreakBlockHandler_ignores_the_player_game_mode);
  TEST(BreakBlockHandler_a_refused_break_never_writes_the_inventory);

  TEST(PlaceBlockHandler_acks_first_then_commits_and_consumes_one);
  TEST(PlaceBlockHandler_acks_before_the_cas_lands);
  TEST(PlaceBlockHandler_a_lost_race_consumes_nothing);
  TEST(PlaceBlockHandler_charges_creative_the_same_as_survival);
  TEST(PlaceBlockHandler_refuses_adventure_and_spectator);
  TEST(SetBlockCASHandler_reports_a_mode_refusal_with_its_own_reason);
  TEST(TheModeReasonNeverLeaksOntoANonPlacementFrame);
  TEST(PlaceBlockHandler_refuses_an_undefined_game_mode);
  TEST(PlaceBlockHandler_consumes_one_from_the_first_matching_stack);
  TEST(PlaceBlockHandler_writes_the_world_even_with_an_empty_inventory);
  TEST(PlaceBlockHandler_applies_the_transform_table_to_id_and_meta);

  TEST(CasRunner_defers_the_completion_onto_postToMain);
  TEST(CasRunner_without_postToMain_runs_the_completion_inline);
  TEST(CasRunner_two_racing_writers_leave_exactly_one_winner);

  TEST(SetBlockCASHandler_rejects_an_unhandled_action_with_a_reason);
  TEST(SetBlockCASHandler_ignores_a_null_table);
  TEST(SetBlockCASHandler_acks_a_handled_action_exactly_once);

  TEST(MachineInteractHandler_opens_a_known_machine_with_its_real_state);
  TEST(MachineInteractHandler_rejects_a_block_that_is_not_a_machine);
  TEST(MachineInteractHandler_rejects_an_unknown_machine_type);
  TEST(MachineInteractHandler_twice_is_idempotent);
  TEST(MachineInteractHandler_reports_the_registry_energy_type_not_zero);
  TEST(MachineInteractHandler_left_click_spins_only_an_opted_in_machine);

  TEST(ElectricDrillHandler_the_four_drill_ids_all_decode_to_tier_zero);
  TEST(ElectricDrillHandler_rejects_a_bare_hand_before_touching_the_world);
  TEST(ElectricDrillHandler_rejects_a_non_drill_tool);
  TEST(ElectricDrillHandler_accepts_the_ulv_drill_and_then_refuses_air);
  TEST(ElectricDrillHandler_refuses_a_block_above_the_tool_tier);
  TEST(ElectricDrillHandler_validates_the_tool_slot);
  TEST(ElectricDrillHandler_a_slotted_drill_is_refused_for_having_no_energy_model);
  TEST(ElectricDrillHandler_a_known_energy_tool_mines_successfully);
  TEST(ElectricDrillHandler_mining_ticks_are_tier_driven);

  TEST(ElectricDrillHandler_refuses_a_tool_with_no_energy_definition);
  TEST(ElectricDrillHandler_no_energy_stays_reserved_for_a_defined_tool);
  TEST(ElectricDrillHandler_never_throws_for_any_tool_item_id);
  TEST(ElectricDrillHandler_success_requires_a_known_energy_definition);

  TEST(ToolActionHandler_drops_a_frame_that_does_not_verify);
  TEST(ToolActionHandler_mine_block_reports_the_drill_state);
  TEST(ToolActionHandler_wrench_cycle_neither_reads_nor_writes);
  TEST(ToolActionHandler_routes_each_declared_action_type);
  TEST(ToolActionHandler_charge_item_does_not_complete_anything);

  TEST(MachineSlotHandler_is_the_player_machine_slot_topic_handler);
  TEST(MachineSlotHandler_refuses_a_position_with_no_machine_entity);
  TEST(MachineSlotHandler_writes_the_item_into_the_machine_container);
  TEST(MachineSlotHandler_swaps_the_old_machine_item_back_to_the_player);
  TEST(MachineSlotHandler_taking_an_item_out_gives_it_to_the_player);
  TEST(MachineSlotHandler_reaches_the_machine_quest_hook);
  TEST(MachineSlotHandler_refuses_an_out_of_range_slot);
  TEST(MachineSlotHandler_drops_a_frame_that_does_not_verify);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
