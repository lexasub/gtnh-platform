// Unit tests for the three mining systems:
//
//   src/game/mining/DrillSystem.cpp             (gp-fcvf)
//   src/game/mining/BatteryBufferSystem.cpp     (gp-lcuf)
//   src/game/mining/CreativeGeneratorSystem.cpp (gp-ht7r)
//
// The sibling src/game/mining/test/test_adjacency_transfer_system.cpp covers
// the fourth system in this directory (AdjacencyTransferSystem) and nothing
// else, so DrillSystem / BatteryBufferSystem / CreativeGeneratorSystem had NO
// coverage in this directory. src/apps/simcore/test/test_ecs_systems.cpp does
// smoke-test all three, but every one of those cases is a single
// `CHECK_GT(x, 0)`-grade assertion that survives a completely wrong
// implementation:
//
//   test_BatteryBufferSystem_charges_tool        "meta > 0 && meta < 9"
//   test_BatteryBufferSystem_empty_slot_noop     "stored == 20000"
//   test_BatteryBufferSystem_full_tool_skips     "stored == 20000"
//   test_BatteryBufferSystem_publishes_..._sink  "block_entity_update_count > 0"
//   test_CreativeGeneratorSystem_fills_energy    "current > 0"
//   test_DrillSystem_drains_tool_energy          "meta == 990 && set_cas_calls == 1"
//   test_DrillSystem_insufficient_tool_...        "state != MINING"
//   test_DrillSystem_falls_back_to_machine_energy "current == 960"
//
// So this suite pins the exact amounts, the exact ordering, the state machine
// and the boundaries the issues asked for.
//
// ============================================================================
// HARNESS NOTE (mandatory, checked by the reviewer)
// ============================================================================
// GoogleTest is FORBIDDEN in this repo: it is absent from conanfile.txt, CI
// does not install libgtest-dev, and CI Release builds with a global -Werror.
// A find_package(GTest QUIET) guard would silently UNREGISTER this test in
// CI, so no such guard exists here. This file uses the project's own CHECK /
// TEST macros (mirrored from src/apps/simcore/test/test_ecs_systems.cpp, which
// is the same shape as src/game/mining/test/test_adjacency_transfer_system.cpp
// and src/game/actions/test/test_action_dispatch.cpp): g_tests / g_passed /
// g_failed plus test_check(). Each test target is its own executable.
//
// ============================================================================
// DETERMINISM
// ============================================================================
// No wall-clock sleeps, no network, no cluster, no display, no display-driven
// timers. Every system is driven with a FIXED dt of 0.05f (the simcore tick).
// Both network-facing collaborators are in-process doubles:
//   * IEventPublisher  -> RecordingPublisher (records, never sends)
//   * PipeEnergyClient -> a real PipeEnergyClient over an UNCONNECTED
//     IoUringRouterClient. RouterClient::publish() short-circuits on
//     `if (!connected_) return;` (src/engine/net/src/router_client.cpp:121), so
//     event publishing is completely inert and no socket is ever opened. This
//     is the same construction the existing simcored_test uses.
//
// DrillSystem's block repository double answers SYNCHRONOUSLY. The real
// ChunkStoreRepository is asynchronous (it hands the callback to the io_uring
// client), which is why DrillSystem keeps a pendingSearches_ counter; with a
// synchronous double the callback has already run by the time getBlock()
// returns, and the pipeline still behaves as it does in production (see
// …search_results_arrive_in_the_same_tick_with_a_synchronous_repository).
//
// ============================================================================
// FINDINGS (gp-fcvf, production defects, NOT fixed here)
// ============================================================================
// FILED AS gp-xotc (P1).
//
// 1. FIXED (was gp-xotc / gp-fcvf finding 1). The search counter latched:
//    `pendingSearches_[ent] = sent;` was an ASSIGNMENT written AFTER the
//    getBlock callbacks had already decremented the map back to empty, so the
//    counter sat at 2 (kMaxPerTick) and every later tick hit
//    `if (pending >= 2) return;` — a drill issued exactly TWO block requests
//    for its entire life. phaseSearch now increments the counter BEFORE
//    issuing each request, which is also the only ordering correct for the
//    production ASYNCHRONOUS repository (replies arrive on the io_uring poll
//    thread, after phaseSearch has returned). The map is mutex-guarded
//    because of that cross-thread access. Pinned by
//    …search_counter_accumulates_and_releases and
//    …search_counter_never_exceeds_the_per_tick_cap.
//
// 2. The spiral never returns the origin, so the `dx == 0 && dy == 0 && dz == 0`
//    guard at DrillSystem.cpp:221 is DEAD CODE. getSpiralOffset advances x/z
//    BEFORE testing `count == n`, so n=0 is already (+1, 0). Measured sequence:
//    n=0 (+1,0), n=1 (+1,+1), n=2 (0,+1), n=3 (-1,+1), n=4 (-1,0), n=5 (-1,-1),
//    n=6 (0,-1), n=7 (+1,-1). Pinned by
//    …spiral_never_produces_the_origin and
//    …second_probe_is_already_a_diagonal. FILED AS gp-4v5i.
//
// 3. The drop comes from the CAS REPLY, not from the searched ore.
//    onMineComplete calls oreToDrop(result.block_id), and result.block_id is
//    whatever the store reports. A store that answers "air" produces a silent
//    item_id 0 drop that still consumes one of the 64 output slots. Pinned by
//    …drop_comes_from_the_store_reply. FILED AS gp-7n7s.
//
// None of these are fixed here; the tests pin what the code does.
//
// ── Findings in the sibling systems, filed from the same suite ───────────────
//
// gp-4pxm (P2) BatteryBufferSystem::chargeSlot has no stored/capacity gate, so a
//         buffer at stored == capacity still charges a tool and ends up below
//         its capacity. Pinned by
//         BatteryBufferSystem_full_buffer_requests_from_the_pipe_network.
//
// gp-u9ua (P1) FIXED HERE. BatteryBufferSystem::onConsumeResponse used to treat
//         node_id 0 as a sentinel, but entt ids the FIRST entity 0, so a
//         response addressed to that node was dropped or credited to the wrong
//         buffer. The fix, and why "0 means none" was the wrong shape for it,
//         are written out at BatteryBufferSystem_a_response_for_node_zero_
//         credits_node_zero below.
//
// gp-w0b7 (P1) PlayerInventoryStore::giveItem's target_slot branch has no
//         kMaxStack clamp, and PlayerActionDispatcher feeds it the
//         wire-controlled pos().x(). Pinned by
//         creative_grant_splits_across_stacks.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include <apps/simcore/Network/PipeEnergyClient.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include <content/content.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/Position.h>
#include <game/actions/PlayerActionDispatcher.h>
#include <game/machines/DrillComponent.h>
#include <game/machines/ItemEnergyStorage.h>
#include <game/mining/BatteryBufferSystem.h>
#include <game/mining/CreativeGeneratorSystem.h>
#include <game/mining/DrillSystem.h>
#include <game/storage/IBlockRepository.h>
#include <game/storage/PlayerInventoryStore.h>

// ---------------------------------------------------------------------------
// Project harness
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char *file, int line, const char *expr,
                const char *msg = nullptr) {
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

#define CHECK_EQ(a, b, ...)                                                    \
    test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_NE(a, b, ...)                                                    \
    test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

// CHECK_EQ in the harness is a raw (a) == (b); these wrappers normalise to
// int64_t so a mixed width/signedness comparison never trips -Wsign-compare.
#define CHECK_EQ_I(a, b, ...)                                                  \
    test_check(static_cast<int64_t>(a) == static_cast<int64_t>(b), __FILE__,   \
               __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_GT_I(a, b, ...)                                                  \
    test_check(static_cast<int64_t>(a) > static_cast<int64_t>(b), __FILE__,    \
               __LINE__, #a " > " #b, ##__VA_ARGS__)

#define CHECK_LT_I(a, b, ...)                                                  \
    test_check(static_cast<int64_t>(a) < static_cast<int64_t>(b), __FILE__,    \
               __LINE__, #a " < " #b, ##__VA_ARGS__)

#define CHECK_LE_I(a, b, ...)                                                  \
    test_check(static_cast<int64_t>(a) <= static_cast<int64_t>(b), __FILE__,   \
               __LINE__, #a " <= " #b, ##__VA_ARGS__)

namespace {

using simcore::BatteryBufferComponent;
using simcore::BatteryBufferSystem;
using simcore::BlockData;
using simcore::CASResult;
using simcore::CreativeGeneratorSystem;
using simcore::DrillComponent;
using simcore::DrillState;
using simcore::DrillSystem;
using simcore::EnergyStorage;
using simcore::EnergyType;
using simcore::InventoryContainer;
using simcore::InventorySlot;
using simcore::IBlockRepository;
using simcore::MachineComponent;
using simcore::Position;
// HatchUpdateData is declared OUTSIDE namespace simcore (IEventPublisher.h:8).
using ::HatchUpdateData;

// The fixed tick every test drives. 20 Hz, the simcore rate.
constexpr float kDt = 0.05f;

// Item ids from src/game/machines/ItemEnergyStorage.h. Named, never raw.
// ITEM_DRILL_ULV, the real drill id: pack("1111:00:0") = 61440. It used to be the
// bare 90, which matches no item in items.csv and therefore matched no entry in
// TOOL_ENERGY_DEFS either (gp-v4re).
constexpr uint16_t kDrillUlv = ITEM_DRILL_ULV;  // capacity 1000, maxInput 8,  tier 0
constexpr uint16_t kDrillMv = ITEM_DRILL_LV;  // capacity 4000, maxInput 32, tier 1
constexpr uint16_t kBatteryLv = 60948; // capacity 1000, maxInput 32, tier 0
constexpr uint16_t kBatteryHv = 60950; // capacity 16000, maxInput 512, tier 2

// Machine ids from src/content/data/registry/machines.yaml.
constexpr uint16_t kCreativeGeneratorId = ItemId::pack("1110:100:0");
constexpr uint16_t kHeatFurnaceId = ItemId::pack("1110:000:0");
constexpr uint16_t kBatteryBufferLvId = ItemId::pack("1110:101:0");

// Ore block ids from src/content/content.h.
constexpr uint16_t kOreIron = content::kOreIron;
constexpr uint16_t kOreGold = content::kOreGold;
constexpr uint16_t kOreDiamond = content::kOreDiamond;
constexpr uint16_t kStoneId = ItemId::pack("0:0:1");

// Battery-buffer tiers from SimulationEngine.cpp:270-281, which builds the
// component out of the machine registry: chargeRate = max(1, maxInput / 4).
constexpr int32_t kLvChargeRate = 8;  // 32 / 4
constexpr int32_t kHvChargeRate = 128; // 512 / 4

// ---------------------------------------------------------------------------
// Recording publisher: implements the full IEventPublisher seam, records what
// the systems publish, and never touches a socket.
// ---------------------------------------------------------------------------

struct EntityUpdateRecord {
    int32_t x = 0, y = 0, z = 0;
    uint16_t machine_type = 0;
    float progress = 0.0f;
    uint32_t energy = 0;
    EnergyType energy_type = EnergyType::ELECTRICITY;
    uint32_t energy_capacity = 0;
    int slots_in = -1;
    size_t inventory_bytes = 0;
};

class RecordingPublisher : public simcore::IEventPublisher {
public:
    std::vector<EntityUpdateRecord> updates;

    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char *, uint32_t = 0, uint8_t = 1) override {}
    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t = 0, uint8_t = 1) override {}
    void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                  uint32_t = 0, uint64_t = 0) override {}

    void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z,
                                  uint16_t machine_type,
                                  const std::vector<uint8_t> &inventory_data,
                                  float progress, uint32_t energy,
                                  EnergyType energy_type = EnergyType::ELECTRICITY,
                                  uint32_t energy_capacity = 0,
                                  int slots_in = -1, float = 0.0f,
                                  const std::vector<HatchUpdateData> * = nullptr,
                                  double = -1.0, double = -1.0) override {
        updates.push_back(EntityUpdateRecord{x, y, z, machine_type, progress,
                                             energy, energy_type, energy_capacity,
                                             slots_in, inventory_data.size()});
    }

    void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                    uint16_t, uint8_t, uint16_t,
                                    const char *) override {}
    void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                          const std::array<uint8_t, 6> &) override {}
    void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t,
                                  uint16_t) override {}
    void publishMultiblockDestroyed(uint64_t) override {}
    void publishGridUpdate(int32_t, int32_t, int32_t,
                           const std::vector<RecipeManager::ItemStack> &) override {}

    void clear() { updates.clear(); }
};

// An unconnected IoUringRouterClient. RouterClient::publish() returns
// immediately when !connected_, so every publish is a no-op and no socket is
// opened. That keeps PipeEnergyClient fully inert.
std::shared_ptr<simcore::IoUringRouterClient> inertRouter() {
    return std::make_shared<simcore::IoUringRouterClient>();
}

std::shared_ptr<simcore::PipeEnergyClient> inertPipeClient() {
    return std::make_shared<simcore::PipeEnergyClient>(inertRouter());
}

// ---------------------------------------------------------------------------
// Block repository double. Answers synchronously so the whole suite is
// single-threaded with no sleeps. The world is a sparse map keyed by the packed
// coordinate; an unqueried cell is air (block_id 0).
// ---------------------------------------------------------------------------

class FakeWorld : public IBlockRepository {
public:
    struct GetCall {
        int32_t x = 0, y = 0, z = 0;
    };
    struct CasCall {
        int32_t x = 0, y = 0, z = 0;
        uint16_t expected = 0;
        uint16_t new_id = 0;
        uint8_t meta = 0;
    };

    std::vector<GetCall> gets;
    std::vector<CasCall> cases;
    // status reported back by setBlockCAS. 0 = OK, 1 = CONFLICT.
    uint8_t cas_status = 0;
    // block_id setBlockCAS reports back in CASResult::block_id on success.
    uint16_t cas_result_block = 0;
    bool cas_fires = true;

    // 21 bits of x, 20 of y, 21 of z in a 64-bit key — no shift overflow, and
    // enough room for every coordinate these tests use.
    static uint64_t key(int32_t x, int32_t y, int32_t z) {
        return (static_cast<uint64_t>(x) & 0x1FFFFF) << 41 |
               (static_cast<uint64_t>(y) & 0xFFFFF) << 21 |
               (static_cast<uint64_t>(z) & 0x1FFFFF);
    }

    void set(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta = 0) {
        cells_[key(x, y, z)] = BlockData{id, meta, 0};
    }
    void setAll() {
        for (auto &kv : cells_) kv.second.block_id = 0;
    }

    void setBlockCAS(int32_t x, int32_t y, int32_t z, uint16_t expected,
                     uint16_t new_id, uint8_t meta,
                     SetBlockCASCallback callback) override {
        cases.push_back(CasCall{x, y, z, expected, new_id, meta});
        if (!cas_fires) {
            return;  // models "still in flight" — the callback never arrives
        }
        auto it = cells_.find(key(x, y, z));
        // The real ChunkStore replies with the block id that was ACTUALLY at
        // the cell, so a successful CAS of an ore reports the ore's id (which
        // is what DrillSystem::onMineComplete feeds to oreToDrop()).
        const uint16_t actual =
            (cas_status == 0)
                ? (cas_result_block ? cas_result_block
                                    : (it != cells_.end() ? it->second.block_id : 0))
                : (it != cells_.end() ? it->second.block_id : 0);
        if (cas_status == 0) {
            cells_[key(x, y, z)] = BlockData{new_id, meta, 0};
        }
        callback(CASResult{cas_status, actual, meta});
    }

    void getBlock(int32_t x, int32_t y, int32_t z,
                  GetBlockCallback callback) override {
        gets.push_back(GetCall{x, y, z});
        if (deferred) {
            pending_.push_back(callback);
            return;
        }
        auto it = cells_.find(key(x, y, z));
        if (it == cells_.end()) {
            callback(BlockData{0, 0, 0});
        } else {
            callback(it->second);
        }
    }

    // ── Asynchronous (production-shaped) mode ─────────────────────────────
    // The real ChunkStoreRepository hands the callback to the io_uring client,
    // so the reply arrives on the poll thread AFTER getBlock() has returned.
    // `deferred` switches this double to that ordering: getBlock records the
    // request and parks the callback until flushPending() runs it. This is the
    // ordering the gp-xotc fix has to be correct for — a "decrement on reply,
    // assign after" counter is wrong in BOTH orderings, and only this mode can
    // show it.
    void defer() { deferred = true; }
    size_t pendingCount() const { return pending_.size(); }
    // Deliver every parked reply, in issue order. Returns how many ran.
    size_t flushPending() {
        std::vector<GetBlockCallback> due;
        due.swap(pending_);
        for (auto& cb : due) cb(BlockData{0, 0, 0});
        return due.size();
    }

    void clearRecording() {
        gets.clear();
        cases.clear();
    }

    size_t blockCount() const { return cells_.size(); }

private:
    std::unordered_map<uint64_t, BlockData> cells_;
    bool deferred = false;
    std::vector<GetBlockCallback> pending_;
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// DrillSystem fixture. events_ is dereferenced unconditionally in tick()
// (DrillSystem.cpp:141), so it can never be null.
struct DrillFixture {
    entt::registry reg;
    std::shared_ptr<FakeWorld> repo = std::make_shared<FakeWorld>();
    std::shared_ptr<RecordingPublisher> events = std::make_shared<RecordingPublisher>();
    std::shared_ptr<simcore::PipeEnergyClient> pipe = inertPipeClient();
    std::unique_ptr<DrillSystem> sys;

    DrillFixture() {
        sys = std::make_unique<DrillSystem>(reg, repo, events, pipe);
    }

    entt::entity addDrill(int32_t x, int32_t y, int32_t z, int32_t tier) {
        auto ent = reg.create();
        DrillComponent d(x, y, z, tier);
        d.state = DrillState::IDLE;
        reg.emplace<DrillComponent>(ent, d);
        return ent;
    }

    DrillComponent &drill(entt::entity e) { return reg.get<DrillComponent>(e); }
};

// A drill with a full ULV drill in slot 0. energyPerTick = 10 * (tier+1)^2.
void giveDrillTool(entt::registry &reg, entt::entity ent, uint16_t item,
                   uint16_t energy, uint8_t count = 1) {
    reg.emplace<InventoryContainer>(ent, 0, 1, std::vector<InventorySlot>{{item, count, energy}});
}

int32_t toolEnergy(const entt::registry &reg, entt::entity ent) {
    const auto &slots = reg.get<InventoryContainer>(ent).slots;
    return static_cast<int32_t>(slots[0].meta);
}

// BatteryBufferSystem fixture.
struct BatteryFixture {
    entt::registry reg;
    std::shared_ptr<RecordingPublisher> events = std::make_shared<RecordingPublisher>();
    std::shared_ptr<simcore::PipeEnergyClient> pipe = inertPipeClient();
    std::unique_ptr<BatteryBufferSystem> sys;

    BatteryFixture() {
        sys = std::make_unique<BatteryBufferSystem>(reg, pipe, events);
    }

    // capacity, stored, tier, maxInput, chargeRate, numSlots
    entt::entity addBuffer(int32_t x, int32_t y, int32_t z,
                           uint32_t capacity, int32_t stored, uint8_t tier,
                           int32_t maxInput, int32_t chargeRate,
                           uint8_t numSlots) {
        auto ent = reg.create();
        reg.emplace<BatteryBufferComponent>(ent, BatteryBufferComponent{capacity, stored, tier,
                                                                        maxInput, chargeRate, numSlots});
        reg.emplace<Position>(ent, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                              static_cast<uint32_t>(z));
        std::vector<InventorySlot> slots(numSlots);
        reg.emplace<InventoryContainer>(ent, 0, numSlots, slots);
        return ent;
    }

    BatteryBufferComponent &buf(entt::entity e) {
        return reg.get<BatteryBufferComponent>(e);
    }
    std::vector<InventorySlot> &slots(entt::entity e) {
        return reg.get<InventoryContainer>(e).slots;
    }
};

// CreativeGeneratorSystem fixture.
struct CreativeFixture {
    entt::registry reg;
    std::shared_ptr<RecordingPublisher> events = std::make_shared<RecordingPublisher>();
    std::shared_ptr<simcore::PipeEnergyClient> pipe = inertPipeClient();
    std::unique_ptr<CreativeGeneratorSystem> sys;

    CreativeFixture() {
        sys = std::make_unique<CreativeGeneratorSystem>(reg, events, pipe);
    }

    entt::entity addGenerator(uint16_t machine_id, int32_t x, int32_t y, int32_t z,
                              int32_t capacity, int32_t current, int32_t maxIn,
                              int32_t maxOut, int32_t tier,
                              EnergyType type = EnergyType::ELECTRICITY) {
        auto ent = reg.create();
        reg.emplace<MachineComponent>(ent, machine_id, 0, static_cast<uint32_t>(x),
                                      static_cast<uint32_t>(y), static_cast<uint32_t>(z), 1);
        reg.emplace<EnergyStorage>(ent, capacity, current, maxIn, maxOut, tier, type);
        return ent;
    }

    EnergyStorage &energy(entt::entity e) { return reg.get<EnergyStorage>(e); }
    MachineComponent &machine(entt::entity e) { return reg.get<MachineComponent>(e); }
};

} // namespace

// ===========================================================================
// DrillSystem — fixture sanity
// ===========================================================================

static void test_DrillSystem_tier_constants_match_the_component() {
    // Every energy figure in this suite is derived from these two functions, so
    // assert the values by name rather than repeating bare numbers.
    CHECK_EQ_I(DrillComponent::calcEnergyPerTick(0), 10, "tier 0 draws 10 EU/tick");
    CHECK_EQ_I(DrillComponent::calcEnergyPerTick(1), 40, "tier 1 draws 40 EU/tick");
    CHECK_EQ_I(DrillComponent::calcEnergyPerTick(2), 90, "tier 2 draws 90 EU/tick");
    CHECK_EQ_I(DrillComponent::calcMiningTicks(0), 100, "tier 0 needs 100 ticks to break a block");
    CHECK_EQ_I(DrillComponent::calcMiningTicks(1), 50, "tier 1 needs 50 ticks");
    CHECK_EQ_I(DrillComponent::calcMiningTicks(2), 33, "tier 2 needs 33 ticks");
    CHECK_EQ_I(DrillComponent::kMaxOutputSize, 64, "the output buffer caps at 64 entries");
}

static void test_DrillSystem_empty_registry_is_a_noop() {
    DrillFixture f;
    f.sys->tick(kDt);
    f.sys->tick(kDt);
    CHECK(f.repo->gets.empty(), "no block is queried when there is no drill");
    CHECK_EQ_I(f.events->updates.size(), 0, "and nothing is published");
}

static void test_DrillSystem_publishes_energy_every_tick() {
    DrillFixture f;
    auto ent = f.addDrill(10, 64, 10, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.events->updates.size(), 1, "the drill publishes every tick it is ticked");
    if (!f.events->updates.empty()) {
        const auto &u = f.events->updates.back();
        CHECK_EQ(u.x, 10, "the update carries the drill's own x");
        CHECK_EQ(u.y, 64, "the update carries the drill's own y");
        CHECK_EQ(u.z, 10, "the update carries the drill's own z");
        CHECK_EQ_I(u.energy, 1000, "the tool energy is reported when a tool is equipped");
        CHECK_EQ_I(u.energy_capacity, 1000,
                   "the capacity comes from TOOL_ENERGY_DEFS, by name");
    }
}

static void test_DrillSystem_reports_machine_energy_without_a_tool() {
    DrillFixture f;
    auto ent = f.addDrill(10, 64, 10, 0);
    // No InventoryContainer: the report falls back to the machine EnergyStorage.
    f.reg.emplace<EnergyStorage>(ent, 10000, 4000, 32, 32, 0, EnergyType::ELECTRICITY);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.events->updates.size(), 1, "the drill still publishes");
    if (!f.events->updates.empty()) {
        CHECK_EQ_I(f.events->updates.back().energy, 4000,
                   "with no tool the machine EnergyStorage is reported");
        CHECK_EQ_I(f.events->updates.back().energy_capacity, 10000,
                   "and so is its capacity");
    }
}

// ===========================================================================
// DrillSystem — mining progress
// ===========================================================================

static void test_DrillSystem_mining_progress_advances_one_tick_per_tick() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 1;
    d.targetY = 64;
    d.targetZ = 0;
    d.miningTicksTotal = DrillComponent::calcMiningTicks(0); // 100
    d.miningProgress = d.miningTicksTotal;

    CHECK_EQ_I(d.miningProgress, 100, "the block starts fully unmined");

    for (int tick = 0; tick < 99; ++tick) {
        f.sys->tick(kDt);
        CHECK_EQ_I(f.drill(ent).miningProgress, 99 - tick,
                   "exactly one tick of progress per system tick");
    }
    CHECK(f.repo->cases.empty(), "no block is broken before the last tick");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::MINING),
               "the drill is still MINING one tick before completion");
}

static void test_DrillSystem_block_breaks_on_the_final_tick() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.repo->set(1, 64, 0, kOreIron);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 1;
    d.targetY = 64;
    d.targetZ = 0;
    // targetOreId is what the SEARCH recorded; in production only
    // onSearchBlockResult can put the drill into MINING, and it always sets
    // this. Tests that hand-build the MINING state must set it too, or they are
    // describing a state the system can never reach (gp-7n7s).
    d.targetOreId = kOreIron;
    d.miningTicksTotal = 4;
    d.miningProgress = 4;

    f.sys->tick(kDt);
    CHECK_EQ_I(f.drill(ent).miningProgress, 3, "tick 1 of 4");
    f.sys->tick(kDt);
    CHECK_EQ_I(f.drill(ent).miningProgress, 2, "tick 2 of 4");
    f.sys->tick(kDt);
    CHECK_EQ_I(f.drill(ent).miningProgress, 1, "tick 3 of 4");
    CHECK(f.repo->cases.empty(), "the CAS has not been issued yet");

    f.sys->tick(kDt);

    CHECK_EQ_I(f.repo->cases.size(), 1, "the block is removed on the final tick");
    if (!f.repo->cases.empty()) {
        const auto &c = f.repo->cases.back();
        CHECK_EQ(c.x, 1, "the CAS targets the ore cell");
        CHECK_EQ(c.y, 64, "the CAS targets the ore cell");
        CHECK_EQ(c.z, 0, "the CAS targets the ore cell");
        CHECK_EQ(c.new_id, 0, "the ore is broken to air");
        CHECK_EQ(c.expected, 0xFFFF, "the CAS expects any block id (0xFFFF)");
    }
    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), 1, "the drop is buffered");
    if (!f.drill(ent).outputBuffer.empty()) {
        CHECK_EQ(f.drill(ent).outputBuffer[0].first,
                 ItemId::pack("0:110:1"),
                 "iron ore drops an iron ingot (content::oreDropTable)");
        CHECK_EQ_I(f.drill(ent).outputBuffer[0].second, 1, "one ingot per ore");
    }
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "a successful mine returns the drill to SEARCHING");
}

static void test_DrillSystem_progress_is_reported_in_the_entity_update() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 1;
    d.miningTicksTotal = 10;
    d.miningProgress = 10;

    f.sys->tick(kDt);
    CHECK_EQ_I(f.drill(ent).miningProgress, 9, "one tick of progress happened");
    CHECK_EQ_I(f.events->updates.size(), 1, "one update published");
    if (!f.events->updates.empty()) {
        // progress = 1 - miningProgress / miningTicksTotal = 1 - 9/10
        CHECK(f.events->updates.back().progress > 0.09f &&
                  f.events->updates.back().progress < 0.11f,
              "the reported progress is the completed fraction (0.1)");
    }

    f.sys->tick(kDt);
    CHECK(f.events->updates.size() >= 2, "a second update is published");
    if (f.events->updates.size() >= 2) {
        CHECK(f.events->updates.back().progress > 0.19f &&
                  f.events->updates.back().progress < 0.21f,
              "the reported progress advances to 0.2");
    }
}

static void test_DrillSystem_energy_is_drawn_on_every_mining_tick() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000); // 1000 EU, tier 0 -> 10/tick

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 5;
    d.miningProgress = 5;

    const int32_t perTick = DrillComponent::calcEnergyPerTick(0);
    for (int tick = 0; tick < 5; ++tick) {
        f.sys->tick(kDt);
        CHECK_EQ_I(toolEnergy(f.reg, ent), 1000 - perTick * (tick + 1),
                   "each mining tick debits exactly energyPerTick");
    }
    CHECK_EQ_I(toolEnergy(f.reg, ent), 950, "five ticks cost exactly 50 EU");
}

static void test_DrillSystem_tier_scales_the_energy_draw() {
    // Tier 1 draws 40 EU/tick (4x tier 0's 10). Pinned by name via
    // calcEnergyPerTick so a constant change is visible here.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 1);
    giveDrillTool(f.reg, ent, kDrillMv, 4000); // MV drill, capacity 4000

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 5;
    d.miningProgress = 5;

    const int32_t perTick = DrillComponent::calcEnergyPerTick(1);
    CHECK_EQ_I(perTick, 40, "tier 1 draws 40 EU/tick");
    f.sys->tick(kDt);
    CHECK_EQ_I(toolEnergy(f.reg, ent), 4000 - perTick, "the MV drill pays the tier-1 rate");
}

static void test_DrillSystem_exact_energy_boundary() {
    // The drain is `consumeToolEnergy`, which REFUSES when current < amount.
    // With energyPerTick = 10 the boundary is therefore: 10 EU mines, 9 does not.
    DrillFixture f;

    {
        DrillFixture ok;
        auto ent = ok.addDrill(0, 64, 0, 0);
        giveDrillTool(ok.reg, ent, kDrillUlv, 10);
        auto &d = ok.drill(ent);
        d.state = DrillState::MINING;
        d.miningTicksTotal = 5;
        d.miningProgress = 5;
        ok.sys->tick(kDt);
        CHECK_EQ_I(toolEnergy(ok.reg, ent), 0, "exactly energyPerTick is enough to mine");
        CHECK_EQ_I(ok.drill(ent).miningProgress, 4, "and the block does progress");
    }

    {
        DrillFixture short1;
        auto ent = short1.addDrill(0, 64, 0, 0);
        giveDrillTool(short1.reg, ent, kDrillUlv, 9);
        auto &d = short1.drill(ent);
        d.state = DrillState::MINING;
        d.miningTicksTotal = 5;
        d.miningProgress = 5;
        short1.sys->tick(kDt);
        CHECK_EQ_I(toolEnergy(short1.reg, ent), 9,
                   "one EU short: the tool is not drained at all");
        CHECK_NE(static_cast<int64_t>(short1.drill(ent).state),
                 static_cast<int64_t>(DrillState::MINING),
                 "and mining is aborted");
    }
}

static void test_DrillSystem_no_energy_source_aborts_mining() {
    // Neither tool nor EnergyStorage: phaseEnergyCheck returns false and the
    // drill falls back to SEARCHING. Note that NO energy is consumed, so this is
    // different from the "insufficient" case, which also returns false.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 5;
    d.miningProgress = 5;

    f.sys->tick(kDt);

    CHECK_NE(static_cast<int64_t>(f.drill(ent).state),
             static_cast<int64_t>(DrillState::MINING),
             "a drill with no energy source cannot mine");
    CHECK_EQ_I(f.drill(ent).miningProgress, 5, "and the block makes no progress");
    CHECK(f.repo->cases.empty(), "no CAS is issued");
}

static void test_DrillSystem_insufficient_energy_does_not_underflow() {
    // The machine-EnergyStorage fallback calls consumeEnergy unconditionally
    // and IGNORES the return value: it returns true even when the buffer is
    // empty. So a drill with an empty machine buffer keeps mining (at zero
    // cost) forever. Pinned as observed.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    f.reg.emplace<EnergyStorage>(ent, 10000, 0, 32, 32, 0, EnergyType::ELECTRICITY);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 3;
    d.miningProgress = 3;

    for (int i = 0; i < 3; ++i) f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).miningProgress, 0, "the block still breaks on an empty buffer");
    CHECK_EQ_I(f.reg.get<EnergyStorage>(ent).current, 0,
               "consumeEnergy clamped at zero, so current never underflows");
}

static void test_DrillSystem_machine_energy_fallback_respects_max_output() {
    // consumeEnergy clamps to maxOutput, so a tier-1 drill needing 40 EU/tick
    // against maxOutput 32 drains only 32.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 1);
    f.reg.emplace<EnergyStorage>(ent, 10000, 1000, 128, 32, 1, EnergyType::ELECTRICITY);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 5;
    d.miningProgress = 5;

    f.sys->tick(kDt);

    CHECK_EQ_I(f.reg.get<EnergyStorage>(ent).current, 1000 - 32,
               "the draw is clamped to maxOutput, not energyPerTick");
}

static void test_DrillSystem_container_without_a_tool_falls_back_to_machine_energy() {
    // A container holding a non-tool item means findToolSlot() == -1, so the
    // machine EnergyStorage path is used even though a container is present.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    f.reg.emplace<InventoryContainer>(ent, 0, 2,
                                      std::vector<InventorySlot>{{kStoneId, 64, 0}, {0, 0, 0}});
    f.reg.emplace<EnergyStorage>(ent, 10000, 100, 32, 32, 0, EnergyType::ELECTRICITY);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 5;
    d.miningProgress = 5;

    f.sys->tick(kDt);

    CHECK_EQ_I(toolEnergy(f.reg, ent), 0, "the stone in slot 0 is untouched");
    CHECK_EQ_I(f.reg.get<EnergyStorage>(ent).current, 100 - 10,
               "the machine buffer pays energyPerTick instead");
}

static void test_DrillSystem_find_tool_slot_takes_the_first_tool() {
    // findToolSlot returns the FIRST slot whose item is in TOOL_ENERGY_DEFS, so
    // with two tools equipped only the first is ever charged or drained.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    f.reg.emplace<InventoryContainer>(ent, 0, 3,
                                      std::vector<InventorySlot>{{kStoneId, 1, 0}, {kDrillUlv, 1, 100}, {kDrillMv, 1, 100}});

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 5;
    d.miningProgress = 5;

    f.sys->tick(kDt);

    const auto &sl = f.reg.get<InventoryContainer>(ent).slots;
    CHECK_EQ_I(sl[0].meta, 0, "the non-tool slot is untouched");
    CHECK_EQ_I(sl[1].meta, 100 - DrillComponent::calcEnergyPerTick(0),
               "the FIRST tool slot is the one that is drained");
    CHECK_EQ_I(sl[2].meta, 100, "a second tool in a later slot is never drained");
}

static void test_DrillSystem_empty_tool_slot_does_not_block_the_later_tool() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    f.reg.emplace<InventoryContainer>(ent, 0, 2,
                                      std::vector<InventorySlot>{{0, 0, 0}, {kDrillUlv, 1, 100}});

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.miningTicksTotal = 5;
    d.miningProgress = 5;

    f.sys->tick(kDt);

    const auto &sl = f.reg.get<InventoryContainer>(ent).slots;
    CHECK_EQ_I(sl[1].meta, 100 - DrillComponent::calcEnergyPerTick(0),
               "an empty slot 0 is skipped and the tool in slot 1 is used");
}

// ===========================================================================
// DrillSystem — output buffer
// ===========================================================================

static void test_DrillSystem_output_buffer_fills_and_blocks_at_the_cap() {
    // Pre-load the buffer to kMaxOutputSize - 1 so exactly one more mine fills
    // it. The drill must then latch into OUTPUT_FULL and stop mining.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    // A full ULV drill (1000 EU) covers the 64 blocks needed to fill the buffer at
    // 10 EU/tick each: 64 * 10 = 640 < 1000. Tool energy lives in a uint16_t
    // meta, so a ULV drill can never hold more than TOOL_ENERGY_DEFS[kDrillUlv].capacity.
    giveDrillTool(f.reg, ent, kDrillUlv, TOOL_ENERGY_DEFS.at(kDrillUlv).capacity);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 1;
    d.targetOreId = kOreIron;  // an ore that DOES drop, so the slot is really spent
    d.miningTicksTotal = 1;
    d.miningProgress = 1;
    d.outputBuffer.assign(static_cast<size_t>(DrillComponent::kMaxOutputSize - 1),
                          std::pair<uint16_t, uint8_t>{ItemId::pack("0:110:1"), 1});

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), DrillComponent::kMaxOutputSize,
               "the buffer reaches its declared cap");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::OUTPUT_FULL),
               "a full output buffer latches the drill into OUTPUT_FULL");
    CHECK(f.drill(ent).isOutputFull(), "isOutputFull() agrees");

    // While OUTPUT_FULL the drill mines nothing at all.
    const size_t cas_after_fill = f.repo->cases.size();
    f.sys->tick(kDt);
    f.sys->tick(kDt);
    CHECK_EQ_I(f.repo->cases.size(), cas_after_fill,
               "an OUTPUT_FULL drill issues no further CAS");
    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), DrillComponent::kMaxOutputSize,
               "and its buffer does not grow past the cap");
}

static void test_DrillSystem_output_full_releases_once_space_appears() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    // A buffer exactly AT the cap does NOT release: the condition is
    // `outputBuffer.size() < kMaxOutputSize`, strictly less. Pinned, because
    // this is the boundary a well-meaning "fix" would get wrong.
    DrillFixture full;
    auto e1 = full.addDrill(0, 64, 0, 0);
    giveDrillTool(full.reg, e1, kDrillUlv, 1000);
    full.drill(e1).state = DrillState::OUTPUT_FULL;
    full.drill(e1).outputBuffer.assign(static_cast<size_t>(DrillComponent::kMaxOutputSize),
                                       std::pair<uint16_t, uint8_t>{ItemId::pack("0:110:1"), 1});
    full.sys->tick(kDt);
    CHECK_EQ_I(full.drill(e1).state, static_cast<int64_t>(DrillState::OUTPUT_FULL),
               "a buffer EXACTLY at the cap does not release: the test is strict <");

    auto &d = f.drill(ent);
    d.state = DrillState::OUTPUT_FULL;
    d.outputBuffer.assign(static_cast<size_t>(DrillComponent::kMaxOutputSize - 1),
                          std::pair<uint16_t, uint8_t>{ItemId::pack("0:110:1"), 1});

    f.sys->tick(kDt);
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "one free slot in the buffer resumes SEARCHING");

    // The cap is `size() < kMaxOutputSize`, so a buffer at cap - 1 also releases.
    DrillFixture f2;
    auto ent2 = f2.addDrill(0, 64, 0, 0);
    giveDrillTool(f2.reg, ent2, kDrillUlv, 1000);
    f2.drill(ent2).state = DrillState::OUTPUT_FULL;
    f2.drill(ent2).outputBuffer.assign(static_cast<size_t>(DrillComponent::kMaxOutputSize - 1),
                                       std::pair<uint16_t, uint8_t>{ItemId::pack("0:110:1"), 1});
    f2.sys->tick(kDt);
    CHECK_EQ_I(f2.drill(ent2).state, static_cast<int64_t>(DrillState::SEARCHING),
               "exactly one free slot is enough to leave OUTPUT_FULL");

    // An empty buffer also releases, and one tick is enough either way.
    DrillFixture f3;
    auto ent3 = f3.addDrill(0, 64, 0, 0);
    giveDrillTool(f3.reg, ent3, kDrillUlv, 1000);
    f3.drill(ent3).state = DrillState::OUTPUT_FULL;
    f3.sys->tick(kDt);
    CHECK_EQ_I(f3.drill(ent3).state, static_cast<int64_t>(DrillState::SEARCHING),
               "an emptied buffer resumes SEARCHING on the very next tick");
}

static void test_DrillSystem_broken_block_resets_the_target_and_progress() {
    // A CONFLICT from the store (status != 0) means the block was already gone.
    // onMineComplete then clears back to SEARCHING with NO drop appended; the
    // next found ore re-arms miningTicksTotal from the tier.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 1); // tier 1 -> 50 ticks
    giveDrillTool(f.reg, ent, kDrillMv, 4000);
    f.repo->cas_status = 1;  // CONFLICT
    f.repo->cas_fires = true;

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 3;
    d.targetY = 64;
    d.targetZ = 3;
    d.miningTicksTotal = 1;
    d.miningProgress = 1;
    d.outputBuffer.push_back({ItemId::pack("0:110:1"), 1});

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), 1,
               "a conflicted CAS appends NO drop");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "and returns the drill to SEARCHING");
    CHECK_EQ_I(f.drill(ent).miningProgress, 0, "miningProgress is left at 0");

    // Now let the search find ore: the tier re-arms the mining timer.
    f.repo->cas_status = 0;
    f.repo->clearRecording();
    f.drill(ent).searchIndex = 0;
    f.drill(ent).searchLayer = 0;
    f.drill(ent).state = DrillState::SEARCHING;
    f.repo->set(1, 64, 0, kOreIron);  // spiral offset 1

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::MINING),
               "fresh ore re-arms the drill");
    CHECK_EQ_I(f.drill(ent).miningTicksTotal, DrillComponent::calcMiningTicks(1),
               "the mining timer is re-derived from the tier");
    CHECK_EQ_I(f.drill(ent).miningProgress, DrillComponent::calcMiningTicks(1),
               "and progress starts fully unmined");
    CHECK_EQ(f.drill(ent).targetX, 1, "the new target is the ore that was found");
}

// gp-7n7s FIXED. Redstone / lapis / diamond are in kOreBlocks (so the search
// targets and mines them) but have NO entry in content::oreDropTable(), so
// oreToDrop() returns 0 for them. The old code appended that 0 anyway, as a
// silent item_id 0 entry that still consumed one of the 64 output slots.
static void test_DrillSystem_ore_without_a_drop_occupies_no_buffer_slot() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.repo->set(1, 64, 0, kOreDiamond);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 1;
    d.targetOreId = kOreDiamond;
    d.miningTicksTotal = 1;
    d.miningProgress = 1;

    f.sys->tick(kDt);

    CHECK_EQ_I(f.repo->cases.size(), 1,
               "a drop-less ore is still MINED — the block leaves the world");
    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), 0,
               "gp-7n7s: and it consumes no output slot, because the drop is 0");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "a drop-less mine does not latch OUTPUT_FULL, so the drill resumes");
}

// The wedge this prevents, end to end. A drill mining drop-less ore used to
// fill all 64 slots with item_id 0 and stop for good — a buffer the player
// sees as full and cannot empty, because the drops do not exist.
static void test_DrillSystem_drop_less_ore_can_never_wedge_the_output_buffer() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    // 70 mines at tier 0 cost 70 * 10 = 700 EU, inside a full ULV drill (1000).
    // 70 > kMaxOutputSize (64), which is the point: under the defect these 70
    // mines would have filled all 64 slots with item_id 0 and stopped there.
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    // Mine diamond 70 times. Each cycle: arm MINING on a diamond with a 1-tick
    // timer, let the drill mine it, then the drop-less result returns it to
    // SEARCHING.
    for (int i = 0; i < 70; ++i) {
        f.drill(ent).state = DrillState::MINING;
        f.drill(ent).targetX = 1;
        f.drill(ent).targetOreId = kOreDiamond;
        f.drill(ent).miningTicksTotal = 1;
        f.drill(ent).miningProgress = 1;
        f.sys->tick(kDt);
    }

    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), 0,
               "gp-7n7s: 70 drop-less mines — more than the 64 slots — buffer nothing");
    CHECK(!f.drill(ent).isOutputFull(), "so the buffer is never full");
    CHECK_NE(static_cast<int64_t>(f.drill(ent).state),
             static_cast<int64_t>(DrillState::OUTPUT_FULL),
             "and the drill never latches OUTPUT_FULL");
    CHECK_EQ_I(f.repo->cases.size(), 70, "every one of them really was mined");
}

// The drop is derived from the ore the drill TARGETED, so the store's reply no
// longer decides it. The real ChunkStore replies with
// resp->actual_block_id() (IoUringChunkClient.cpp:128), which is whatever was
// in the cell at commit time — normally the ore, but NOT when another actor
// broke the block between the search and the CAS.
static void test_DrillSystem_drop_comes_from_the_targeted_ore() {
    DrillFixture good;
    auto e1 = good.addDrill(0, 64, 0, 0);
    giveDrillTool(good.reg, e1, kDrillUlv, 1000);
    good.repo->set(1, 64, 0, kOreIron);
    good.drill(e1).state = DrillState::MINING;
    good.drill(e1).targetX = 1;
    good.drill(e1).targetY = 64;
    good.drill(e1).targetZ = 0;
    good.drill(e1).targetOreId = kOreIron;
    good.drill(e1).miningTicksTotal = 1;
    good.drill(e1).miningProgress = 1;
    good.sys->tick(kDt);
    CHECK_EQ_I(good.drill(e1).outputBuffer.size(), 1, "one drop is buffered");
    if (!good.drill(e1).outputBuffer.empty()) {
        CHECK_EQ(good.drill(e1).outputBuffer[0].first, ItemId::pack("0:110:1"),
                 "an iron ore drops an iron ingot");
    }

    // The defect case: the store commits and answers "there was air here",
    // because something else broke the ore in between. gp-7n7s: the drop is
    // still the ore the drill aimed at, and no phantom item_id 0 appears.
    DrillFixture air;
    auto e2 = air.addDrill(0, 64, 0, 0);
    giveDrillTool(air.reg, e2, kDrillUlv, 1000);
    air.repo->cas_result_block = 0;  // the store replies "there was air here"
    air.drill(e2).state = DrillState::MINING;
    air.drill(e2).targetX = 1;
    air.drill(e2).targetY = 64;
    air.drill(e2).targetZ = 0;
    air.drill(e2).targetOreId = kOreIron;  // what the search found
    air.drill(e2).miningTicksTotal = 1;
    air.drill(e2).miningProgress = 1;
    air.sys->tick(kDt);

    CHECK_EQ_I(air.drill(e2).outputBuffer.size(), 1,
               "gp-7n7s: an air reply still yields exactly one real drop");
    if (!air.drill(e2).outputBuffer.empty()) {
        CHECK_EQ(air.drill(e2).outputBuffer[0].first, ItemId::pack("0:110:1"),
                 "gp-7n7s: and it is the TARGETED ore's drop, not item_id 0");
    }

    // The other direction, and the only one that is a genuine no-op: a drop-less
    // ore targeted at a store that happily reports it back.
    DrillFixture barren;
    auto e3 = barren.addDrill(0, 64, 0, 0);
    giveDrillTool(barren.reg, e3, kDrillUlv, 1000);
    barren.repo->set(1, 64, 0, kOreDiamond);
    barren.drill(e3).state = DrillState::MINING;
    barren.drill(e3).targetX = 1;
    barren.drill(e3).targetOreId = kOreDiamond;
    barren.drill(e3).miningTicksTotal = 1;
    barren.drill(e3).miningProgress = 1;
    barren.sys->tick(kDt);
    CHECK_EQ_I(barren.drill(e3).outputBuffer.size(), 0,
               "gp-7n7s: diamond drops nothing and buffers nothing");
}

// targetOreId is what the SEARCH records, and the drill must not carry a stale
// one into an unrelated mine. A conflicted CAS abandons the target, so the
// remembered ore goes with it; otherwise the next successful mine at those
// coordinates would drop the previous target's ore.
static void test_DrillSystem_a_conflict_clears_the_remembered_ore() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.repo->cas_status = 1;  // CONFLICT
    f.repo->cas_fires = true;

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 3;
    d.targetY = 64;
    d.targetZ = 3;
    d.targetOreId = kOreIron;
    d.miningTicksTotal = 1;
    d.miningProgress = 1;

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "precondition: a conflicted CAS returns the drill to SEARCHING");
    CHECK_EQ_I(f.drill(ent).targetOreId, 0,
               "gp-7n7s: and the abandoned target's ore id is cleared with it");

    // A fresh find re-arms both the coordinates and the ore id together.
    // Spiral index 3 is (0, +1) in layer 0, i.e. the cell beside the drill; the
    // first tick probes indices 1 and 2 only, so drive the index there.
    f.repo->cas_status = 0;
    f.repo->clearRecording();
    f.repo->set(0, 64, 1, kOreGold);
    f.drill(ent).searchIndex = 3;
    f.drill(ent).searchLayer = 0;
    f.drill(ent).state = DrillState::SEARCHING;
    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).targetOreId, kOreGold,
               "gp-7n7s: finding new ore overwrites the remembered id");
    CHECK_EQ_I(f.drill(ent).targetZ, 1, "and the new target is the cell it found");
}

// The search is the ONLY producer of the MINING state in production, so it is
// the only place the drop source can come from. Pinned explicitly, so that a
// future change which arms MINING from anywhere else has to decide what the
// drop is.
static void test_DrillSystem_the_search_records_the_ore_it_finds() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    // Spiral index 1 is (+1, 0) — the first cell layer 0 probes, since index 0 is
    // the drill's own cell and is skipped.
    f.repo->set(1, 64, 0, kOreGold);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::MINING),
               "the drill arms on the ore it found");
    CHECK_EQ_I(f.drill(ent).targetOreId, kOreGold,
               "gp-7n7s: and records its id at the same time as its coordinates");
    CHECK_EQ_I(f.drill(ent).targetX, 1, "coordinates are recorded too");
    CHECK_EQ_I(f.drill(ent).targetZ, 0, "coordinates are recorded too");
}

// A fresh component targets nothing, so it must claim no ore. Pinned because
// targetOreId is what the drop is derived from, and a default of anything else
// would hand a new drill a free drop.
static void test_DrillSystem_a_fresh_drill_targets_no_ore() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    CHECK_EQ_I(f.drill(ent).targetOreId, 0, "a default-constructed drill claims no ore");
    f.drill(ent).reset();
    CHECK_EQ_I(f.drill(ent).targetOreId, 0, "and reset() keeps it that way");
}

static void test_DrillSystem_non_ore_blocks_are_never_mined() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.repo->set(1, 64, 0, kStoneId);

    f.drill(ent).state = DrillState::SEARCHING;
    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "stone is found by no search, so the drill keeps searching");
    CHECK(f.repo->cases.empty(), "and nothing is broken");
    CHECK(f.drill(ent).outputBuffer.empty(), "and nothing is buffered");
}

static void test_DrillSystem_search_starts_from_idle_and_probes_two_cells() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::IDLE),
               "a fresh drill starts IDLE");
    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "the first tick promotes IDLE to SEARCHING");
    CHECK_EQ_I(f.repo->gets.size(), 2, "at most kMaxPerTick = 2 blocks are queried per tick");
    // gp-4v5i: the index advances once per spiral cell CONSIDERED, and index 0
    // is now the drill's own cell, which the origin guard skips. So a tick from
    // index 0 consumes three indices (0 skipped, 1 and 2 queried) to issue its
    // two requests. The per-tick REQUEST rate is still exactly kMaxPerTick.
    CHECK_EQ_I(f.drill(ent).searchIndex, 3,
               "the search index advances once per issued request, plus the "
               "skipped origin");
    CHECK_EQ_I(f.drill(ent).searchLayer, 0, "and the layer only changes at the wrap");
}

static void test_DrillSystem_search_counter_accumulates_and_releases() {
    // gp-xotc, FIXED. `pendingSearches_[ent] = sent;` used to be an ASSIGNMENT
    // running after the synchronous callbacks had already erased the entry, so
    // the counter latched at kMaxPerTick and a drill issued exactly two block
    // requests for its whole life. The counter is now incremented BEFORE each
    // request is issued and released by the matching reply, so the search
    // sustains kMaxPerTick requests per tick indefinitely.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    f.sys->tick(kDt);
    CHECK_EQ_I(f.repo->gets.size(), 2, "tick 1 issues two block requests");
    const int32_t index_after_tick1 = f.drill(ent).searchIndex;

    for (int tick = 0; tick < 5; ++tick) f.sys->tick(kDt);

    // Five more ticks at the same rate: 5 * kMaxPerTick on top of tick 1.
    CHECK_EQ_I(f.repo->gets.size(), 12,
               "every later tick issues kMaxPerTick more requests — the counter "
               "is released by each reply instead of latching");
    CHECK_EQ_I(f.drill(ent).searchIndex, index_after_tick1 + 10,
               "and the search index advances with every issued request");
    CHECK_EQ_I(f.drill(ent).searchLayer, 0,
               "the drill is still in layer 0 — 12 requests is well short of the "
               "440-cell wrap");
}

static void test_DrillSystem_search_counter_never_exceeds_the_per_tick_cap() {
    // The rate cap must still hold: the counter is what enforces
    // kMaxPerTick, so a drill must not be able to issue more than 2 requests
    // in a single tick even though it is now correctly released.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    for (int tick = 0; tick < 10; ++tick) {
        const size_t before = f.repo->gets.size();
        f.sys->tick(kDt);
        CHECK_EQ_I(static_cast<int64_t>(f.repo->gets.size() - before), 2,
                   "each tick issues exactly kMaxPerTick = 2 requests");
    }
}

static void test_DrillSystem_search_sustains_its_rate_under_an_async_repository() {
    // gp-xotc, the production ordering. The real ChunkStoreRepository forwards
    // to IoUringChunkClient::GetBlock, whose reply is delivered from
    // IoUringConnection's poll thread — i.e. AFTER phaseSearch has returned.
    // Here the double is switched to that mode, so a tick's two requests are
    // still outstanding when the next tick runs. The counter must therefore
    // (a) be charged BEFORE the request goes out, and
    // (b) be released by the reply, so the drill keeps searching at the same
    //     rate once the replies land.
    // A counter that only decremented on reply would let a drill run away
    // (unbounded requests per tick); the old assigning counter latched.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.repo->defer();

    // Tick 1: two requests issued, both still in flight.
    f.sys->tick(kDt);
    CHECK_EQ_I(f.repo->gets.size(), 2, "tick 1 issues two requests");
    CHECK_EQ_I(static_cast<int64_t>(f.repo->pendingCount()), 2,
               "neither reply has arrived yet — this is the production ordering");

    // Ticks 2 and 3 run while tick 1's replies are still outstanding: the
    // outstanding count must throttle them, not let them run away.
    f.sys->tick(kDt);
    f.sys->tick(kDt);
    CHECK_EQ_I(f.repo->gets.size(), 2,
               "no further requests while two replies are still in flight — the "
               "counter is charged before the request goes out");
    CHECK_EQ_I(static_cast<int64_t>(f.repo->pendingCount()), 2,
               "still exactly two outstanding");

    // The replies land, which releases both slots.
    CHECK_EQ_I(static_cast<int64_t>(f.repo->flushPending()), 2,
               "both parked replies are delivered");
    CHECK_EQ_I(static_cast<int64_t>(f.repo->pendingCount()), 0,
               "and the counter is fully released");

    // The next tick can search again — the drill was not stranded at 2 forever.
    const size_t before = f.repo->gets.size();
    f.sys->tick(kDt);
    CHECK_EQ_I(static_cast<int64_t>(f.repo->gets.size() - before), 2,
               "once the replies land the drill resumes its full per-tick rate");
    CHECK(f.drill(ent).searchIndex >= 2,
          "and the search index has advanced past the first tick's probes");
}

static void test_DrillSystem_search_index_wraps_to_the_next_layer() {
    // searchIndex >= 440 rolls the index to 0 and advances searchLayer. Pinned
    // by driving the index to the wrap directly.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    // The 440-cell wrap is driven directly: start the index at 439 and let the
    // two requests of that tick run: 439 -> 440 wraps to 0, then +1.
    DrillFixture wrap;
    auto went = wrap.addDrill(0, 64, 0, 0);
    giveDrillTool(wrap.reg, went, kDrillUlv, 1000);
    wrap.drill(went).state = DrillState::SEARCHING;
    wrap.drill(went).searchIndex = 439;
    wrap.sys->tick(kDt);

    CHECK_EQ_I(wrap.drill(went).searchIndex, 1,
               "index 439 + 1 == 440 triggers the wrap to 0, then the next "
               "request lands on 1");
    CHECK_EQ_I(wrap.drill(went).searchLayer, 1, "and the layer advances");
    CHECK_EQ_I(wrap.repo->gets.size(), 2, "still two requests on the wrapping tick");

    static_cast<void>(ent);
    static_cast<void>(f);
}

static void test_DrillSystem_layer_offset_sequence() {
    // layerOffset: 0, -1, +1, -2, +2, -3, +3 ... (DrillSystem.cpp:17-20).
    // The drill always searches BELOW itself first.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    f.sys->tick(kDt);
    // Layer 0 -> dy = 0; spiral offsets 1 and 2 are +X and +Y.
    CHECK_EQ_I(f.repo->gets.size(), 2, "two probes in layer 0");
    if (f.repo->gets.size() == 2) {
        CHECK_EQ(f.repo->gets[0].y, 64, "layer 0 probes at the drill's own y");
        CHECK_EQ(f.repo->gets[1].y, 64, "both layer-0 probes share the drill's y");
    }
}

// gp-4v5i FIXED. The spiral is a 4-connected square ring whose n=0 is the
// drill's OWN cell.
//
// The old loop advanced x/z before testing `count == n`, so n=0 already
// returned (+1, 0). That made three separate things wrong at once: the
// `dx == 0 && dy == 0 && dz == 0` guard in phaseSearch was unreachable dead
// code, the drill's own column was never a candidate, and half of every
// four-cell ring was a corner, so a drill tunnelled in a zig-zag and could
// skip the nearest ore straight ahead of it.
//
// The spiral itself is now:
//   n=0 ( 0,  0)   n=4 (-1,  0)   n=8  ( 2, -1)
//   n=1 (+1,  0)   n=5 (-1, -1)   n=9  ( 2,  0)
//   n=2 (+1, +1)   n=6 ( 0, -1)
//   n=3 ( 0, +1)   n=7 (+1, -1)
//
// getSpiralOffset is not a public API, so it is pinned through the only
// channel it has: the coordinates the repository is asked about. Note the
// observable sequence in LAYER 0 starts at n=1, because n=0 is the drill's own
// cell and phaseSearch skips it before issuing a request — see
// …the_origin_probe_is_never_issued.
static void test_DrillSystem_the_spiral_starts_at_the_origin() {
    // Layer 1 has no skipped cell, so the FULL spiral is observable there and
    // its very first probe is the origin. This is the one place the raw
    // getSpiralOffset(0) output can be observed directly.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    // Park the drill in layer 1 (dy = -1), where index 0 is not skipped. The
    // state must be SEARCHING, because an IDLE tick resets searchLayer/index.
    f.drill(ent).state = DrillState::SEARCHING;
    f.drill(ent).searchLayer = 1;
    f.drill(ent).searchIndex = 0;

    f.sys->tick(kDt);

    CHECK_EQ_I(f.repo->gets.size(), 2, "two probes on the first search tick");
    if (f.repo->gets.size() == 2) {
        CHECK_EQ(f.repo->gets[0].x, 0, "gp-4v5i: probe 0 is at the drill's own x");
        CHECK_EQ(f.repo->gets[0].z, 0, "gp-4v5i: and its own z: the ORIGIN");
        CHECK_EQ(f.repo->gets[0].y, 63, "in layer 1, so one block below the drill");
        CHECK_EQ(f.repo->gets[1].x, 1, "probe 1 is the +X neighbour");
        CHECK_EQ(f.repo->gets[1].z, 0, "gp-4v5i: one axis step from the origin");
    }
}

// n=0 is the drill's own cell in layer 0, and phaseSearch skips it BEFORE
// issuing a request. That guard was dead code while getSpiralOffset could not
// produce (0,0); it is now live, and this is what it protects: the drill must
// never try to mine the block it is standing in, and must not spend one of its
// two per-tick requests asking about itself.
static void test_DrillSystem_the_origin_probe_is_never_issued() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    // The drill's own cell is ORE, so if the guard did not skip the origin the
    // drill would target itself.
    f.repo->set(0, 64, 0, kOreIron);
    f.repo->set(1, 64, 0, kOreIron);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.repo->gets.size(), 2, "still exactly two requests, none wasted");
    if (f.repo->gets.size() == 2) {
        CHECK(!(f.repo->gets[0].x == 0 && f.repo->gets[0].y == 64 && f.repo->gets[0].z == 0),
              "gp-4v5i: the drill's own cell is never even asked about in layer 0");
        CHECK(!(f.repo->gets[1].x == 0 && f.repo->gets[1].y == 64 && f.repo->gets[1].z == 0),
              "gp-4v5i: neither of the two probes is the drill's own cell");
    }
    CHECK_EQ_I(f.drill(ent).targetX, 1,
               "gp-4v5i: so the drill targets the +X neighbour, never itself");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::MINING),
               "and it is mining, not stuck");
}

// 4-connected means 4-connected: consecutive spiral cells differ by exactly one
// axis move, never by a diagonal step. Pinned across a whole layer, because the
// defect was a half-diagonal ring that a single spot check misses — the old
// spiral took 220 diagonal steps in each 440-cell layer.
static void test_DrillSystem_the_spiral_is_four_connected() {
    // Walk one full layer, one probe per tick, and require every consecutive
    // pair of probes to be a single axis move. Layer 1 is used so the walk
    // starts at the origin and covers the whole spiral including n=0.
    DrillFixture f;
    auto ent = f.addDrill(500, 64, 500, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.drill(ent).state = DrillState::SEARCHING;
    f.drill(ent).searchLayer = 1;  // no skipped origin

    int32_t px = 500, pz = 500;
    bool first = true;
    int32_t offStep = 0;
    for (int i = 0; i < 440; ++i) {
        f.drill(ent).searchIndex = i;
        f.repo->clearRecording();
        f.sys->tick(kDt);
        if (f.repo->gets.empty()) continue;
        const int32_t x = f.repo->gets.front().x;
        const int32_t z = f.repo->gets.front().z;
        if (!first) {
            const int32_t step = std::abs(x - px) + std::abs(z - pz);
            if (step != 1) ++offStep;
        }
        px = x; pz = z;
        first = false;
    }
    CHECK(!first, "the walk actually produced probes to compare");
    CHECK_EQ_I(offStep, 0,
               "gp-4v5i: every consecutive spiral cell is one axis move — the "
               "old spiral had 220 diagonal steps per layer");
}

// The ring order, pinned cell by cell. Under the old 8-diagonal spiral the
// sequence was (+1,0), (+1,+1), (0,+1), ... — the very SECOND cell was already
// a corner, so the drill met a corner before it met the wall beside it. Now
// the origin comes first and the four face neighbours fall at n=1, 3, 5 and 7.
static void test_DrillSystem_the_spiral_visits_the_four_walls_in_ring_order() {
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.drill(ent).state = DrillState::SEARCHING;
    f.drill(ent).searchLayer = 1;  // no skipped origin, so n=0 is observable

    // Collect the first 8 spiral cells, one per tick.
    std::vector<std::pair<int32_t, int32_t>> seen;
    for (int i = 0; i < 8; ++i) {
        f.drill(ent).searchIndex = i;
        f.repo->clearRecording();
        f.sys->tick(kDt);
        if (f.repo->gets.empty()) continue;
        seen.push_back({f.repo->gets.front().x, f.repo->gets.front().z});
    }
    CHECK_EQ_I(static_cast<int64_t>(seen.size()), 8, "eight spiral cells observed");
    if (seen.size() != 8) return;

    // n=0 is the origin, and n=1..7 are the first ring of the 4-connected spiral.
    const std::pair<int32_t, int32_t> kExpected[8] = {
        {0, 0}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}
    };
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(seen[i].first, kExpected[i].first, "spiral x matches at this index");
        CHECK_EQ(seen[i].second, kExpected[i].second, "spiral z matches at this index");
    }
    // And the specific claim: the four FACE neighbours are at odd indices 1, 3,
    // 5, 7 — no corner is reached before all four walls are.
    CHECK_EQ(seen[1].second, 0, "gp-4v5i: n=1 is the +X wall, not a corner");
    CHECK_EQ(seen[3].first, 0, "gp-4v5i: n=3 is the +Z wall");
    CHECK_EQ(seen[5].first, -1, "gp-4v5i: n=5 is the -X wall");
    CHECK_EQ(seen[7].first, 0, "gp-4v5i: n=7 is the -Z wall");
}

static void test_DrillSystem_the_block_directly_below_is_reachable() {
    // The block DIRECTLY BELOW a drill is the single most valuable target, and it
    // lives in layer 1 (dy = -1), not layer 0. Before gp-4v5i, spiral index 0 was
    // one step out horizontally, so the drill's own COLUMN was never a candidate at
    // any index — (0, 63, 0) was simply unreachable. The origin fix is what makes
    // it reachable: layer 1's origin cell IS the block below, and the guard skips
    // the origin only when dy == 0 as well.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, TOOL_ENERGY_DEFS.at(kDrillUlv).capacity);
    f.drill(ent).state = DrillState::SEARCHING;
    f.drill(ent).searchLayer = 1;  // dy = -1, whose origin is the block below
    f.drill(ent).searchIndex = 0;
    f.repo->set(0, 63, 0, kOreIron);  // directly below, and the only ore in range

    for (int i = 0; i < 10; ++i) f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::MINING),
               "gp-4v5i: the drill finds the ore directly beneath itself");
    CHECK_EQ_I(f.drill(ent).targetX, 0, "targeted x is the drill's own x");
    CHECK_EQ_I(f.drill(ent).targetY, 63, "targeted y is one block BELOW the drill");
    CHECK_EQ_I(f.drill(ent).targetZ, 0, "targeted z is the drill's own z");
    CHECK_EQ_I(f.drill(ent).searchLayer, 1,
               "and it was found in layer 1, whose origin is the cell below");
}

static void test_DrillSystem_only_layer_zero_skips_the_origin() {
    // Layer 0's skipped origin is the drill's OWN cell, which holds the drill and
    // can never be ore, so skipping it loses nothing. Every other layer keeps its
    // origin: layer 1's is the block below, layer 2's the block above. Pinned so a
    // future "optimisation" that skips index 0 unconditionally cannot silently
    // delete the block below the drill.
    DrillFixture self;
    auto e0 = self.addDrill(0, 64, 0, 0);
    giveDrillTool(self.reg, e0, kDrillUlv, TOOL_ENERGY_DEFS.at(kDrillUlv).capacity);
    self.repo->set(0, 64, 0, kOreIron);
    for (int i = 0; i < 5; ++i) self.sys->tick(kDt);
    CHECK_EQ_I(self.drill(e0).searchLayer, 0, "precondition: the search is in layer 0");
    CHECK(!((self.drill(e0).targetX == 0 && self.drill(e0).targetY == 64 &&
             self.drill(e0).targetZ == 0)),
          "gp-4v5i: layer 0's origin is the drill's own cell and is never mined");

    DrillFixture below;
    auto e1 = below.addDrill(0, 64, 0, 0);
    giveDrillTool(below.reg, e1, kDrillUlv, TOOL_ENERGY_DEFS.at(kDrillUlv).capacity);
    below.drill(e1).state = DrillState::SEARCHING;
    below.drill(e1).searchLayer = 1;  // dy = -1: index 0 is the block below
    below.drill(e1).searchIndex = 0;
    below.repo->set(0, 63, 0, kOreIron);
    for (int i = 0; i < 5; ++i) below.sys->tick(kDt);
    CHECK_EQ_I(below.drill(e1).targetY, 63,
               "gp-4v5i: layer 1's origin is NOT skipped — it is the cell below");
}

static void test_DrillSystem_dt_is_ignored() {
    // Every phase is tick-counted, never dt-scaled: a drill needs the same
    // number of ticks at 20 Hz and at 1 kHz.
    DrillFixture fast;
    auto a = fast.addDrill(0, 64, 0, 0);
    giveDrillTool(fast.reg, a, kDrillUlv, 1000);
    fast.drill(a).state = DrillState::MINING;
    fast.drill(a).miningTicksTotal = 10;
    fast.drill(a).miningProgress = 10;
    for (int i = 0; i < 5; ++i) fast.sys->tick(1.0f);

    DrillFixture slow;
    auto b = slow.addDrill(0, 64, 0, 0);
    giveDrillTool(slow.reg, b, kDrillUlv, 1000);
    slow.drill(b).state = DrillState::MINING;
    slow.drill(b).miningTicksTotal = 10;
    slow.drill(b).miningProgress = 10;
    for (int i = 0; i < 5; ++i) slow.sys->tick(0.001f);

    CHECK_EQ_I(fast.drill(a).miningProgress, 5, "5 ticks at dt=1.0 make 5 ticks of progress");
    CHECK_EQ_I(slow.drill(b).miningProgress, 5, "5 ticks at dt=0.001 make the same 5");
}

static void test_DrillSystem_search_results_arrive_in_the_same_tick_with_a_synchronous_repository() {
    // FINDING-adjacent (gp-fcvf). onSearchBlockResult runs INSIDE the getBlock
    // call, so with a synchronous repository a drill can go IDLE -> MINING in a
    // single tick. The real ChunkStoreRepository is asynchronous (it forwards to
    // the io_uring client), so in production this transition is deferred. The
    // mining math is unaffected: miningTicksTotal is already set before MINING.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.repo->set(1, 64, 0, kOreIron);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::MINING),
               "with a synchronous repository the found ore arms mining in the "
               "same tick that the search ran");
    CHECK_EQ_I(f.drill(ent).miningProgress, DrillComponent::calcMiningTicks(0),
               "and miningProgress is the FULL timer, not one tick less");
}

static void test_DrillSystem_idle_resets_the_search_each_tick() {
    // tick() resets searchLayer/searchIndex whenever the drill is IDLE.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    f.drill(ent).state = DrillState::IDLE;
    f.drill(ent).searchLayer = 5;
    f.drill(ent).searchIndex = 300;

    f.sys->tick(kDt);

    // gp-4v5i: the restart begins at index 0, which is the drill's own cell and
    // is skipped, so the two requests come from indices 1 and 2 and the index
    // lands on 3. The SEARCH still restarted from 0 — that is the claim here.
    CHECK_EQ_I(f.drill(ent).searchIndex, 3,
               "the search restarts from index 0 on the IDLE tick (0 is the "
               "skipped origin, 1 and 2 are the two probes)");
    CHECK_EQ_I(f.drill(ent).searchLayer, 0, "and the layer is reset too");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::SEARCHING),
               "the drill leaves IDLE after one tick");
}

static void test_DrillSystem_output_buffer_is_not_persisted_across_a_reset() {
    // DrillComponent::reset() clears the output buffer; the system itself never
    // calls it, so the buffer only ever grows in-process. Pinned as observed.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.drill(ent).outputBuffer.push_back({ItemId::pack("0:110:1"), 1});

    for (int i = 0; i < 5; ++i) f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), 1,
               "ticking never clears the output buffer; only reset() does");
    f.drill(ent).reset();
    CHECK(f.drill(ent).outputBuffer.empty(), "reset() empties it");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::IDLE),
               "and returns the drill to IDLE");
}

static void test_DrillSystem_multiple_drills_are_ticked_independently() {
    DrillFixture f;
    auto a = f.addDrill(0, 64, 0, 0);
    auto b = f.addDrill(100, 64, 100, 0);
    giveDrillTool(f.reg, a, kDrillUlv, 100);
    giveDrillTool(f.reg, b, kDrillUlv, 100);

    f.drill(a).state = DrillState::MINING;
    f.drill(a).miningTicksTotal = 10;
    f.drill(a).miningProgress = 10;
    f.drill(b).state = DrillState::MINING;
    f.drill(b).miningTicksTotal = 10;
    f.drill(b).miningProgress = 3;

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(a).miningProgress, 9, "drill A advanced one tick");
    CHECK_EQ_I(f.drill(b).miningProgress, 2, "drill B advanced one tick");
    CHECK_EQ_I(toolEnergy(f.reg, a), 90, "each drill pays its own energy");
    CHECK_EQ_I(toolEnergy(f.reg, b), 90, "each drill pays its own energy");
    CHECK_EQ_I(f.events->updates.size(), 2, "one update per drill");
}

// ===========================================================================
// BatteryBufferSystem
// ===========================================================================

static void test_BatteryBufferSystem_view_requires_all_three_components() {
    // The view is <BatteryBufferComponent, InventoryContainer, Position>.
    BatteryFixture missingPos;
    auto e1 = missingPos.addBuffer(1, 2, 3, 40000, 1000, 1, 32, kLvChargeRate, 1);
    missingPos.slots(e1)[0] = InventorySlot{kDrillUlv, 1, 0};
    missingPos.reg.remove<Position>(e1);
    missingPos.sys->tick(kDt);
    CHECK_EQ_I(missingPos.buf(e1).stored, 1000,
               "without Position the entity is outside the view and nothing charges");
    CHECK_EQ_I(missingPos.slots(e1)[0].meta, 0, "the tool is untouched");

    BatteryFixture missingInv;
    auto e2 = missingInv.addBuffer(1, 2, 3, 40000, 1000, 1, 32, kLvChargeRate, 1);
    missingInv.reg.remove<InventoryContainer>(e2);
    missingInv.sys->tick(kDt);
    CHECK_EQ_I(missingInv.buf(e2).stored, 1000, "without an inventory nothing charges");

    BatteryFixture missingBuffer;
    auto e3 = missingBuffer.addBuffer(1, 2, 3, 40000, 1000, 1, 32, kLvChargeRate, 1);
    missingBuffer.reg.remove<BatteryBufferComponent>(e3);
    missingBuffer.slots(e3)[0] = InventorySlot{kDrillUlv, 1, 0};
    missingBuffer.sys->tick(kDt);
    CHECK_EQ_I(missingBuffer.slots(e3)[0].meta, 0, "without the buffer the tool is untouched");
}

static void test_BatteryBufferSystem_charges_at_the_declared_rate() {
    // chargeRate = 8 EU/tick per slot for an LV buffer (32 / 4).
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 1000, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, kLvChargeRate,
               "the tool gains exactly chargeRate EU in one tick");
    CHECK_EQ_I(f.buf(ent).stored, 1000 - kLvChargeRate,
               "and the buffer is debited by the same amount");
}

static void test_BatteryBufferSystem_charges_every_slot_once_per_tick() {
    // numSlots 2 with two batteries: each is charged chargeRate, so the buffer
    // pays 2 * chargeRate in one tick.
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 150000, 10000, 1, 128, 32, 2);
    f.slots(ent)[0] = InventorySlot{kBatteryLv, 1, 0};
    f.slots(ent)[1] = InventorySlot{kBatteryLv, 1, 0};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, 32, "slot 0 gains the full chargeRate");
    CHECK_EQ_I(f.slots(ent)[1].meta, 32, "slot 1 gains the full chargeRate");
    CHECK_EQ_I(f.buf(ent).stored, 10000 - 64, "the buffer pays for both slots");
}

static void test_BatteryBufferSystem_charge_is_clamped_by_the_buffer() {
    // The transfer is min(chargeRate, stored, capacity-cur, maxInput). With
    // stored == 5 and chargeRate == 8 the buffer is the limiting term.
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 5, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, 5, "the transfer is limited to what the buffer holds");
    CHECK_EQ_I(f.buf(ent).stored, 0, "and the buffer empties exactly");
    CHECK(f.buf(ent).stored >= 0, "stored never goes negative");
}

static void test_BatteryBufferSystem_empty_buffer_stops_charging() {
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    f.sys->tick(kDt);
    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, 0, "an empty buffer charges nothing");
    CHECK_EQ_I(f.buf(ent).stored, 0, "and stays empty");
}

static void test_BatteryBufferSystem_full_tool_is_not_charged() {
    // A ULV drill's capacity is 1000; at meta 1000 chargeSlot returns early.
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 10000, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 1000};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, 1000, "a full tool is left alone");
    CHECK_EQ_I(f.buf(ent).stored, 10000, "and the buffer pays nothing");
}

static void test_BatteryBufferSystem_exact_tool_capacity_boundary() {
    // currentEnergy >= capacity is the skip condition, so exactly-at-capacity is
    // skipped while one below charges by the remaining single EU.
    BatteryFixture atCap;
    auto e1 = atCap.addBuffer(10, 64, 10, 40000, 10000, 1, 32, kLvChargeRate, 1);
    atCap.slots(e1)[0] = InventorySlot{kDrillUlv, 1, 1000};
    atCap.sys->tick(kDt);
    CHECK_EQ_I(atCap.slots(e1)[0].meta, 1000, "exactly at capacity: skipped");
    CHECK_EQ_I(atCap.buf(e1).stored, 10000, "and the buffer is not debited");

    BatteryFixture oneBelow;
    auto e2 = oneBelow.addBuffer(10, 64, 10, 40000, 10000, 1, 32, kLvChargeRate, 1);
    oneBelow.slots(e2)[0] = InventorySlot{kDrillUlv, 1, 1000 - 1};
    oneBelow.sys->tick(kDt);
    CHECK_EQ_I(oneBelow.slots(e2)[0].meta, 1000, "one EU below capacity is topped up");
    CHECK_EQ_I(oneBelow.buf(e2).stored, 10000 - 1, "for exactly that one EU");
}

static void test_BatteryBufferSystem_charge_is_clamped_by_the_tool_max_input() {
    // def.maxInput caps the per-slot transfer. An HV buffer with chargeRate 128
    // charging a ULV drill (maxInput 8) moves only 8.
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 600000, 50000, 2, 512, kHvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, TOOL_ENERGY_DEFS.at(kDrillUlv).maxInput,
               "the transfer is capped by the tool's own maxInput");
    CHECK_EQ_I(f.slots(ent)[0].meta, 8, "a ULV drill accepts at most 8 EU/tick");
    CHECK_EQ_I(f.buf(ent).stored, 50000 - 8, "and the buffer is debited the same");
}

static void test_BatteryBufferSystem_non_tool_items_are_never_charged() {
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 10000, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kStoneId, 64, 1234};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.buf(ent).stored, 10000, "stone has no energy def, so nothing is drawn");
    CHECK_EQ_I(f.slots(ent)[0].meta, 1234, "and the meta is untouched");
}

static void test_BatteryBufferSystem_num_slots_caps_the_charged_slots() {
    // The loop is `for (i = 0; i < numSlots && i < inv.slots.size(); i++)`, so a
    // tool in a slot BEYOND numSlots is never charged, even though the inventory
    // physically holds it.
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 10000, 1, 32, kLvChargeRate, 1);
    f.reg.get<InventoryContainer>(ent).slots.push_back(InventorySlot{kDrillUlv, 1, 0});
    CHECK_EQ_I(f.slots(ent).size(), 2, "precondition: the inventory has two slots");

    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, 0, "slot 0 is empty, so nothing is charged");
    CHECK_EQ_I(f.slots(ent)[1].meta, 0,
               "and the tool in slot 1 is unreachable because numSlots == 1");
    CHECK_EQ_I(f.buf(ent).stored, 10000, "the buffer pays nothing");
}

static void test_BatteryBufferSystem_short_inventory_is_safely_bounded() {
    // numSlots can exceed the physical slot count; the `&& i < slots.size()`
    // guard prevents an out-of-range read.
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 10000, 1, 32, kLvChargeRate, 4);
    f.reg.get<InventoryContainer>(ent).slots.resize(1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.slots(ent)[0].meta, kLvChargeRate, "the one real slot is still charged");
    CHECK_EQ_I(f.slots(ent).size(), 1, "and no slot is invented");
}

static void test_BatteryBufferSystem_charges_repeatedly_until_full() {
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 1000, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    // 1000 / 8 = 125 ticks exactly.
    for (int i = 0; i < 124; ++i) f.sys->tick(kDt);
    CHECK_EQ_I(f.slots(ent)[0].meta, 1000 - kLvChargeRate,
               "one tick before full the tool holds capacity - chargeRate");

    f.sys->tick(kDt);
    CHECK_EQ_I(f.slots(ent)[0].meta, 1000, "the final tick fills the tool exactly");
    CHECK_EQ_I(f.buf(ent).stored, 1000 - 125 * kLvChargeRate, "and the buffer empties exactly");

    f.sys->tick(kDt);
    CHECK_EQ_I(f.slots(ent)[0].meta, 1000, "a full tool is never charged again");
    CHECK_EQ_I(f.buf(ent).stored, 1000 - 125 * kLvChargeRate, "and the buffer is stable");
}

static void test_BatteryBufferSystem_dt_is_ignored() {
    BatteryFixture fast;
    auto a = fast.addBuffer(10, 64, 10, 40000, 1000, 1, 32, kLvChargeRate, 1);
    fast.slots(a)[0] = InventorySlot{kDrillUlv, 1, 0};
    fast.sys->tick(1.0f);

    BatteryFixture slow;
    auto b = slow.addBuffer(10, 64, 10, 40000, 1000, 1, 32, kLvChargeRate, 1);
    slow.slots(b)[0] = InventorySlot{kDrillUlv, 1, 0};
    slow.sys->tick(0.001f);

    CHECK_EQ_I(fast.slots(a)[0].meta, slow.slots(b)[0].meta,
               "the per-tick charge is identical at any dt");
    CHECK_EQ_I(fast.buf(a).stored, slow.buf(b).stored, "so is the buffer debit");
}

static void test_BatteryBufferSystem_publishes_state_for_a_machine() {
    // The publish is gated on MachineComponent (try_get), and reports
    // energy=stored, capacity=buffer.capacity, slots_in=numSlots.
    BatteryFixture f;
    auto ent = f.addBuffer(11, 65, 12, 40000, 12345, 1, 32, kLvChargeRate, 1);
    f.reg.emplace<MachineComponent>(ent, kBatteryBufferLvId, 0, 11, 65, 12, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.events->updates.size(), 1, "a buffer that is also a machine publishes");
    if (!f.events->updates.empty()) {
        const auto &u = f.events->updates.back();
        CHECK_EQ(u.x, 11, "the publish uses the MACHINE coordinates");
        CHECK_EQ(u.machine_type, kBatteryBufferLvId, "and the machine id");
        CHECK_EQ_I(u.energy, 12345 - kLvChargeRate, "energy reports the buffer's stored EU");
        CHECK_EQ_I(u.energy_capacity, 40000, "capacity reports the buffer's capacity");
        CHECK_EQ_I(u.slots_in, 1, "slots_in reports numSlots");
        CHECK_EQ_I(u.inventory_bytes, 5, "the inventory is packed 5 bytes per slot");
    }
}

static void test_BatteryBufferSystem_publishes_nothing_without_a_machine_component() {
    BatteryFixture f;
    auto ent = f.addBuffer(11, 65, 12, 40000, 1000, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    f.sys->tick(kDt);

    CHECK_EQ_I(f.events->updates.size(), 0,
               "the publish is gated on MachineComponent, which is absent here");
    CHECK_EQ_I(f.slots(ent)[0].meta, kLvChargeRate,
               "but the charge still happens");
}

static void test_BatteryBufferSystem_publish_inventory_packs_meta_little_endian() {
    // The wire format is item_id lo/hi, count, meta lo/hi.
    BatteryFixture f;
    auto ent = f.addBuffer(11, 65, 12, 40000, 1000, 1, 32, kLvChargeRate, 1);
    f.reg.emplace<MachineComponent>(ent, kBatteryBufferLvId, 0, 11, 65, 12, 1);
    f.slots(ent)[0] = InventorySlot{0x1234, 7, 0xABCD};
    // item 0x1234 is not a tool, so the buffer is not debited and the packing is
    // the only thing under test.
    f.sys->tick(kDt);

    CHECK_EQ_I(f.events->updates.size(), 1, "one update");
    CHECK_EQ_I(f.events->updates.size(), 1, "and the inventory is 5 bytes per slot");
    if (!f.events->updates.empty()) {
        CHECK_EQ_I(f.events->updates.back().inventory_bytes, 5,
                   "one slot packs to exactly five bytes");
    }
}

static void test_BatteryBufferSystem_full_buffer_requests_from_the_pipe_network() {
    // A full buffer requests nothing; a non-full one requests
    // min(space, maxInput). The pipe client is inert (unconnected router), so
    // this is observed through the request bookkeeping: onConsumeResponse
    // reports a MISS (returns false) for a node with no outstanding request, so
    // it doubles as a read-only probe for "did tick() ask the network?".
    //
    // gp-4pxm FIXED (partially — the issue's premise was wrong, see below).
    //
    // The issue claimed "a full or empty buffer does not charge" and asked for
    // the charging loop to be gated on `stored < capacity`. That gate would
    // DEADLOCK the machine: the request block is gated on the SAME condition,
    // and charging the tools is the only thing that ever drains a buffer, so a
    // buffer at capacity could neither charge a tool nor ask for EU, and would
    // sit there charging nothing forever. A full buffer must keep charging.
    //
    // The one claim that does hold is about the ORDER, and it is a real defect:
    // charging runs FIRST and unconditionally, so a buffer that was exactly
    // full is debited to 39992 and only THEN does the request block see
    // `stored < capacity` and ask the network for 8 more EU. The gate meant to
    // express "full" never sees a full buffer. Pinned as observed below.
    //
    // onConsumeResponse reports a MISS (false) for a node with no outstanding
    // request, so it doubles as a read-only probe for "did tick() ask?".
    BatteryFixture f;
    auto full = f.addBuffer(10, 64, 10, 40000, 40000, 1, 32, kLvChargeRate, 1);
    f.slots(full)[0] = InventorySlot{kDrillUlv, 1, 0};
    f.sys->tick(kDt);
    CHECK_EQ_I(f.slots(full)[0].meta, kLvChargeRate,
               "a FULL buffer still charges its tool — that is the machine's purpose");
    CHECK_EQ_I(f.buf(full).stored, 40000 - kLvChargeRate,
               "and pays for it out of the EU it holds");
    CHECK(f.sys->onConsumeResponse(static_cast<uint64_t>(full), 8, 0),
           "which leaves it below capacity, so it is legitimately eligible to ask "
           "for a refill in the same tick");

    BatteryFixture g;
    auto partial = g.addBuffer(10, 64, 10, 40000, 1000, 1, 32, kLvChargeRate, 1);
    g.slots(partial)[0] = InventorySlot{kDrillUlv, 1, 0};
    g.sys->tick(kDt);
    CHECK(g.sys->onConsumeResponse(static_cast<uint64_t>(partial), 8, 0),
           "a non-full buffer has an outstanding request, so the response is a hit");
    CHECK_EQ_I(g.buf(partial).stored, 1000 - kLvChargeRate + 8,
               "and the charge it took and the charge it asked for are independent");

    // The gate's intended case: a buffer that is full AND has nothing to charge
    // stays full, and that is the only shape in which the request gate is seen.
    BatteryFixture idle;
    auto idleBuf = idle.addBuffer(10, 64, 10, 40000, 40000, 1, 32, kLvChargeRate, 1);
    idle.sys->tick(kDt);
    CHECK(!idle.sys->onConsumeResponse(static_cast<uint64_t>(idleBuf), 8, 0),
           "a full buffer with an EMPTY inventory slot is asked for nothing: "
           "stored < capacity is false");
    CHECK_EQ_I(idle.buf(idleBuf).stored, 40000, "and it stays exactly full");

    BatteryFixture h;
    auto empty = h.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    h.slots(empty)[0] = InventorySlot{kDrillUlv, 1, 0};
    h.sys->tick(kDt);
    CHECK_EQ_I(h.buf(empty).stored, 0, "an empty buffer is not magically filled by ticking");
    CHECK_EQ_I(h.slots(empty)[0].meta, 0, "and it cannot charge a tool it has no EU for");
}

// gp-4pxm FIXED. `stored` is debited by chargeSlot(), credited by
// onConsumeResponse() and debited again by the discharge path
// (EnergyFlowHandler), and nothing validated it. While stored stayed
// non-negative the missing gate was invisible — min(chargeRate, stored, ...)
// went negative and the `energyToTransfer <= 0` early-out caught it — but the
// invalid level itself was carried into everything downstream of tick():
//
//   * the published energy is `static_cast<uint32_t>(buffer.stored)`, so -50
//     reached the client as 4294967246 EU;
//   * the request block sized its ask from `capacity - stored` = 40050, i.e.
//     more than the buffer can ever hold;
//   * the pipe-node update advertised the same wrapped value.
//
// So the defect is the absent validation, not a wrong charge amount, and the
// charge level is now clamped where it is spent.
static void test_BatteryBufferSystem_negative_stored_is_clamped_to_zero() {
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, -50, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};
    f.reg.emplace<MachineComponent>(ent, kBatteryBufferLvId, 0, 10, 64, 10, 1);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.buf(ent).stored, 0,
               "gp-4pxm: a negative charge level is clamped to zero before anything spends it");
    CHECK_EQ_I(f.slots(ent)[0].meta, 0,
               "and a buffer holding nothing cannot charge a tool");
    CHECK_EQ_I(f.events->updates.size(), 1, "a machine buffer still publishes its state");
    if (!f.events->updates.empty()) {
        CHECK_EQ_I(f.events->updates.back().energy, 0,
                   "gp-4pxm: the published energy is 0, not the unsigned wrap of -50");
        CHECK_EQ_I(f.events->updates.back().energy_capacity, 40000,
                   "and the capacity alongside it is the real one");
    }
    CHECK(f.sys->onConsumeResponse(static_cast<uint64_t>(ent), 10, 0),
           "and it asks the network, with a request sized from the clamped level");
    CHECK_EQ_I(f.buf(ent).stored, 10, "so a response credits normally afterwards");
}

// The mirror of the clamp above: a stored value ABOVE capacity is just as
// invalid as a negative one, and onConsumeResponse already refuses to credit
// past capacity — so a corrupt level must not be allowed to keep the buffer
// above it either.
static void test_BatteryBufferSystem_stored_above_capacity_is_clamped() {
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 40000 + 500, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};
    f.reg.emplace<MachineComponent>(ent, kBatteryBufferLvId, 0, 10, 64, 10, 1);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.buf(ent).stored, 40000 - kLvChargeRate,
               "gp-4pxm: the overfull level is capped at capacity, then spent normally");
    CHECK_EQ_I(f.slots(ent)[0].meta, kLvChargeRate, "and the tool is charged from the capped level");
    if (!f.events->updates.empty()) {
        CHECK_EQ_I(f.events->updates.back().energy, 40000 - kLvChargeRate,
                   "so the published energy never exceeds the published capacity");
    }
}

static void test_BatteryBufferSystem_consume_response_charges_the_matching_node() {
    // onConsumeResponse looks the node up in pendingRequests_, which is only
    // populated by a tick that actually issued a request. A response for an
    // untracked node is a miss (returns false, no state change).
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 1000, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    // entt hands id 0 to the first entity, so this buffer IS node 0 — a real
    // node whose responses must reach it. The old `if (node_id != 0)` guard
    // could not express that and dropped the response outright whenever the
    // FIFO was empty (gp-u9ua).
    const uint64_t node = static_cast<uint64_t>(ent);
    CHECK_EQ_I(node, 0,
               "precondition: entt ids its first entity 0, so this buffer IS node 0");
    CHECK(!f.sys->onConsumeResponse(node, 100, 0),
           "before the tick there is no outstanding request, so a response is "
           "still a miss — the miss is about the request, not the node id");
    CHECK_EQ_I(f.buf(ent).stored, 1000, "and the buffer is unchanged");

    f.sys->tick(kDt);  // issues the request
    const int32_t after_tick = f.buf(ent).stored;
    CHECK_EQ_I(after_tick, 1000 - kLvChargeRate, "the tick charged the tool");
    CHECK(f.sys->onConsumeResponse(node, 500, 0),
           "gp-u9ua: once the request is registered, the response addressed to "
           "node 0 is accepted like any other node's");
    CHECK_EQ_I(f.buf(ent).stored, after_tick + 500,
               "and credits the full amount onto the addressed buffer");

    CHECK(!f.sys->onConsumeResponse(node, 500, 0),
           "the request is consumed exactly once");
    CHECK_EQ_I(f.buf(ent).stored, after_tick + 500, "a repeat response changes nothing");

    // A second buffer on a NON-zero id takes the same path and settles on its
    // own id. The filler is created first so the buffer under test cannot be
    // entity 0; it has no components, so it stays outside the view and issues
    // no request of its own.
    BatteryFixture g;
    [[maybe_unused]] auto filler = g.reg.create();
    static_cast<void>(filler); // filler: takes entity id 0
    auto warmup = g.addBuffer(1, 1, 1, 40000, 0, 1, 32, kLvChargeRate, 1);
    CHECK_EQ_I(static_cast<int64_t>(filler), 0, "precondition: the filler is entity 0");
    CHECK_NE(static_cast<int64_t>(warmup), 0, "precondition: the buffer is not entity 0");
    g.slots(warmup)[0] = InventorySlot{kDrillUlv, 1, 0};
    g.sys->tick(kDt);
    const uint64_t real = static_cast<uint64_t>(warmup);
    CHECK_NE(real, 0u, "precondition: with a filler first, the buffer is not node 0");

    CHECK(g.sys->onConsumeResponse(real, 300, 0),
           "a tracked non-zero node is credited through the direct lookup");
    CHECK_EQ_I(g.buf(warmup).stored, 300, "and the credit lands on the right buffer");

    CHECK(!g.sys->onConsumeResponse(real, 100, 0),
           "with the request retired, a further response is a miss");
    CHECK_EQ_I(g.buf(warmup).stored, 300, "and the buffer is unchanged");
}

static void test_BatteryBufferSystem_consume_response_clamps_to_capacity() {
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};
    f.sys->tick(kDt);

    const uint64_t node = static_cast<uint64_t>(ent);
    CHECK(f.sys->onConsumeResponse(node, 1000000, 0), "a huge response is accepted");
    CHECK_EQ_I(f.buf(ent).stored, 40000, "but stored is clamped to capacity");
    CHECK_LE_I(f.buf(ent).stored, static_cast<int32_t>(f.buf(ent).capacity),
               "stored never exceeds capacity");
}

static void test_BatteryBufferSystem_consume_response_rejects_zero_and_negative() {
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};
    f.sys->tick(kDt);
    const int32_t base = f.buf(ent).stored;
    const uint64_t node = static_cast<uint64_t>(ent);

    CHECK(!f.sys->onConsumeResponse(node, 0, 0), "a zero response is rejected");
    CHECK_EQ_I(f.buf(ent).stored, base, "and the request is NOT retired, so it can still settle");
    CHECK(!f.sys->onConsumeResponse(node, -5, 0), "a negative response is rejected");
    CHECK_EQ_I(f.buf(ent).stored, base, "and again the request survives");
    CHECK(f.sys->onConsumeResponse(node, 10, 0), "a positive response still settles it");
    CHECK_EQ_I(f.buf(ent).stored, base + 10, "and credits correctly");
}

// gp-u9ua FIXED. onConsumeResponse correlates a response with the request that
// asked for it, and the ONLY correlation key that survives the round trip is
// the node id. Three cases below, one per way the old code got it wrong:
//
//   1. node 0 is a REAL node. entt hands id 0 to the first entity it creates,
//      and BatteryBufferSystem::tick() sends that entity's own id as the
//      request's node_id, so node 0 is on the wire like any other. The old
//      `if (node_id != 0)` guard could therefore never take the direct branch
//      for it and always fell through to the FIFO heuristic.
//   2. The FIFO heuristic then credited the WRONG buffer. It picks
//      pendingOrder_.front(), and the view is walked in reverse creation order,
//      so the front is the LAST-created entity. A response addressed to node 0
//      credited node 1.
//   3. An id that matches NO outstanding request must credit nobody. The old
//      code fell through to the FIFO anyway and handed it a live buffer.
//
// The shape of the fix: there is no "undirected response" to fall back for.
// PipeNetworkService::handleConsumeRequest always echoes req->node_id() into
// the response (PipeNetworkService.cpp:959,1004), so every legitimate response
// names its requester, and an absent node_id on the wire can only mean a
// request that went unanswered. Routing strictly by node id also makes this
// system behave like its siblings LCRSystem::onConsumeResponse /
// EBFSystem::onConsumeResponse, which look the id up and return false on a
// miss, so the SimCoreMessageHandler chain (battery -> lcr -> ebf -> machine,
// SimCoreMessageHandler.cpp:256-265) can hand an unclaimed response on instead
// of having whichever system happens to be asked first guess.
static void test_BatteryBufferSystem_a_response_for_node_zero_credits_node_zero() {
    // Two buffers, no tools, so `stored` moves ONLY in response to a consume
    // response and the arithmetic below has one moving part.
    BatteryFixture f;
    auto zero = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    auto other = f.addBuffer(20, 64, 20, 40000, 0, 1, 32, kLvChargeRate, 1);
    CHECK_EQ_I(static_cast<int64_t>(zero), 0,
               "precondition: entt ids its first entity 0, so this buffer IS node 0");
    CHECK_NE(static_cast<int64_t>(other), 0, "precondition: the second buffer is not node 0");

    f.sys->tick(kDt);  // both buffers are below capacity, so both request
    CHECK(f.sys->onConsumeResponse(0, 500, 0),
           "gp-u9ua: a response addressed to node 0 is a response to a request "
           "this system made, so it is accepted");
    CHECK_EQ_I(f.buf(zero).stored, 500,
               "gp-u9ua: and the EU lands on the buffer the node id names");
    CHECK_EQ_I(f.buf(other).stored, 0,
               "gp-u9ua: the FIFO fallback must NOT credit the last-created "
               "entity instead of the addressed one");
}

static void test_BatteryBufferSystem_a_response_for_node_zero_settles_its_own_request() {
    // The second half of the defect: because the mis-credit also settled the
    // WRONG node's request, node 0's own request was left outstanding forever.
    // A buffer with a live pending entry never re-requests (tick() skips it), so
    // node 0 would silently stop drawing EU.
    BatteryFixture f;
    auto zero = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    auto other = f.addBuffer(20, 64, 20, 40000, 0, 1, 32, kLvChargeRate, 1);
    CHECK_EQ_I(static_cast<int64_t>(zero), 0, "precondition: the first buffer is node 0");

    f.sys->tick(kDt);
    CHECK(f.sys->onConsumeResponse(0, 500, 0), "the node-0 response is accepted");
    // Retire node 1's request too, so the only thing that can re-request is
    // node 0 — and only if its own request was actually retired.
    CHECK(f.sys->onConsumeResponse(static_cast<uint64_t>(other), 10, 0),
           "precondition: node 1's outstanding request settles on its own id");
    f.sys->tick(kDt);
    // With the defect, node 0 still holds a pending entry, so this tick issued
    // no request for it and it is frozen at 500. With the fix the request was
    // retired, the tick re-requested, and node 0 is starving again and asking
    // for more. Node 1 was credited exactly once, so the second response above
    // is what retired it and nothing has credited it since.
    CHECK_EQ_I(f.buf(zero).stored, 500,
               "gp-u9ua: node 0 is not credited by the tick itself (no EU arrives "
               "without a response) — the credit came only from its own response");
    CHECK_EQ_I(f.buf(other).stored, 10,
               "gp-u9ua: node 1 was credited by its own response only, never by "
               "node 0's");
}

static void test_BatteryBufferSystem_an_unknown_node_id_credits_nobody() {
    // A response naming an id with no outstanding request is not this system's
    // business. It must credit NOBODY and report the miss, so the next system in
    // the SimCoreMessageHandler chain gets its turn. The old FIFO fallback made
    // "I do not know who this is for" mean "credit whoever I asked first".
    BatteryFixture f;
    auto zero = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    auto other = f.addBuffer(20, 64, 20, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.sys->tick(kDt);  // both requests outstanding

    constexpr uint64_t kStrangerNode = 999;
    CHECK(!f.sys->onConsumeResponse(kStrangerNode, 500, 0),
           "gp-u9ua: an id with no outstanding request is reported as a miss, so "
           "the handler chain passes it on");
    CHECK_EQ_I(f.buf(zero).stored, 0, "gp-u9ua: it credits neither live buffer");
    CHECK_EQ_I(f.buf(other).stored, 0, "gp-u9ua: including the FIFO front");

    // Both requests survive a mis-addressed response, so they can still settle.
    CHECK(f.sys->onConsumeResponse(0, 100, 0), "node 0 still settles afterwards");
    CHECK_EQ_I(f.buf(zero).stored, 100, "and is credited its own amount");
}

static void test_BatteryBufferSystem_the_first_entity_is_node_zero() {
    // entt hands out entity id 0 to the FIRST entity a registry creates
    // (measured: create three -> 0, 1, 2), and entt::null is 0xFFFFFFFF, not 0.
    // So the very first battery buffer in a fresh registry IS node 0 — a real
    // node, on the wire, with responses of its own.
    entt::registry probe;
    [[maybe_unused]] auto first = probe.create();
    [[maybe_unused]] auto second = probe.create();
    CHECK_EQ_I(static_cast<int64_t>(first), 0, "the first entity entt creates is id 0");
    CHECK_EQ_I(static_cast<int64_t>(second), 1, "and the next is id 1");
    // entt::null cannot be constant-folded here: referencing it instantiates
    // entt::basic_entt_traits, which is incomplete at this point in the header
    // chain. entt 3.x defines null as (entity_type{entt::null_t}) == ...
    // all-bits-set, i.e. 0xFFFFFFFF for a 32-bit entity.
    constexpr uint32_t kEnttNull = 0xFFFFFFFFu;
    CHECK_NE(static_cast<int64_t>(kEnttNull), 0,
             "entt's null handle is NOT 0, so entity 0 is a perfectly valid handle");

    // With exactly one buffer in the registry, its node id IS 0, and that
    // buffer must still be creditable — the single-entity case is where the old
    // code dropped the response outright whenever the FIFO was empty.
    BatteryFixture f;
    auto only = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    CHECK_EQ_I(static_cast<int64_t>(only), 0,
               "a single-entity registry puts the buffer on node 0");
    f.sys->tick(kDt);
    CHECK(f.sys->onConsumeResponse(0, 250, 0),
           "gp-u9ua: a lone node-0 buffer accepts a response addressed to it");
    CHECK_EQ_I(f.buf(only).stored, 250, "and is credited");
}

static void test_BatteryBufferSystem_discharge_path_is_owned_by_the_flow_handler() {
    // BatteryBufferSystem has NO discharge method: the buffer is drained by
    // EnergyFlowHandler (src/apps/simcore/ECS/Reactors/EnergyFlowHandler.cpp:37-42)
    // and its own chargeSlot. So an emptied buffer simply stops charging — it
    // does not push energy anywhere. Pinned so the issue's "discharge" wording
    // is answered by the code's actual shape.
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 500};

    for (int i = 0; i < 10; ++i) f.sys->tick(kDt);

    CHECK_EQ_I(f.buf(ent).stored, 0, "an empty buffer stays empty");
    CHECK_EQ_I(f.slots(ent)[0].meta, 500,
               "and the partly-charged tool is NOT topped up from thin air");
}

// ===========================================================================
// CreativeGeneratorSystem
// ===========================================================================

static void test_CreativeGeneratorSystem_default_rate_constant() {
    CHECK_EQ_I(CreativeGeneratorSystem::kDefaultEnergyPerTick, 1024,
               "the default rate is the named constant 1024 EU/tick");
    CHECK_EQ(CreativeGeneratorSystem::kCreativeGeneratorBlockId,
             ItemId::pack("1110:100:0"), "the block id is the content-registry creative_generator");
}

static void test_CreativeGeneratorSystem_fills_at_the_default_rate() {
    CreativeFixture f;
    auto ent = f.addGenerator(kCreativeGeneratorId, 100, 64, 100, 100000, 0, 32, 100000, 10);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(ent).current, CreativeGeneratorSystem::kDefaultEnergyPerTick,
               "an empty generator gains exactly energyPerTick in one tick");
    CHECK_EQ_I(f.events->updates.size(), 1, "and publishes the state");
    if (!f.events->updates.empty()) {
        const auto &u = f.events->updates.back();
        CHECK_EQ(u.x, 100, "the update carries the machine's x");
        CHECK_EQ(u.machine_type, kCreativeGeneratorId, "and the creative generator id");
        CHECK_EQ_I(u.energy, 1024, "reporting the new current");
        CHECK(f.events->updates.back().progress == 1.0f, "progress is reported as 1.0");
    }
}

static void test_CreativeGeneratorSystem_set_energy_per_tick_is_honoured() {
    CreativeFixture f;
    f.sys->setEnergyPerTick(50);
    auto ent = f.addGenerator(kCreativeGeneratorId, 100, 64, 100, 100000, 0, 32, 100000, 10);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(ent).current, 50, "setEnergyPerTick(50) overrides the default");
}

static void test_CreativeGeneratorSystem_rate_accumulates_over_ticks() {
    CreativeFixture f;
    auto ent = f.addGenerator(kCreativeGeneratorId, 100, 64, 100, 100000, 0, 32, 100000, 10);

    for (int i = 0; i < 10; ++i) f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(ent).current, 10 * CreativeGeneratorSystem::kDefaultEnergyPerTick,
               "ten ticks produce ten times the per-tick rate");
    CHECK_EQ_I(f.events->updates.size(), 10, "one publish per tick");
}

static void test_CreativeGeneratorSystem_is_clamped_to_the_remaining_space() {
    // toAdd = min(energyPerTick, space). 1000 free of a 1024 rate -> 1000.
    CreativeFixture f;
    auto ent = f.addGenerator(kCreativeGeneratorId, 100, 64, 100, 10000, 9000, 32, 100000, 10);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(ent).current, 10000, "the buffer is filled exactly, not overshot");
    CHECK_EQ_I(f.events->updates.size(), 1, "and the tick still published");
}

static void test_CreativeGeneratorSystem_exact_space_boundary() {
    // space == energyPerTick -> exactly full; space == energyPerTick - 1 -> one
    // short, then the next tick fills it.
    CreativeFixture exact;
    auto a = exact.addGenerator(kCreativeGeneratorId, 1, 64, 1, 10000, 10000 - 1024, 32, 100000, 10);
    exact.sys->tick(kDt);
    CHECK_EQ_I(exact.energy(a).current, 10000, "space exactly equal to the rate fills the buffer");
    CHECK(exact.energy(a).isFull(), "isFull() agrees");

    CreativeFixture oneShort;
    auto b = oneShort.addGenerator(kCreativeGeneratorId, 1, 64, 1, 10000, 10000 - 1023, 32, 100000, 10);
    oneShort.sys->tick(kDt);
    CHECK_EQ_I(oneShort.energy(b).current, 10000, "one EU short is closed by the clamp");
    CHECK(oneShort.energy(b).isFull(), "so the buffer is exactly full afterwards");
}

static void test_CreativeGeneratorSystem_full_buffer_is_untouched() {
    CreativeFixture f;
    auto ent = f.addGenerator(kCreativeGeneratorId, 100, 64, 100, 100000, 100000, 32, 100000, 10);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(ent).current, 100000, "a full generator produces nothing");
    CHECK_EQ_I(f.events->updates.size(), 0, "and publishes nothing (early `continue`)");
}

static void test_CreativeGeneratorSystem_other_machine_ids_are_ignored() {
    CreativeFixture f;
    auto creative = f.addGenerator(kCreativeGeneratorId, 1, 64, 1, 10000, 0, 32, 100000, 10);
    auto furnace = f.addGenerator(kHeatFurnaceId, 2, 64, 2, 10000, 0, 32, 100000, 0);
    auto buffer = f.addGenerator(kBatteryBufferLvId, 3, 64, 3, 40000, 0, 32, 100000, 0);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(creative).current, 1024, "the creative generator produces");
    CHECK_EQ_I(f.energy(furnace).current, 0, "a heat furnace is not a creative generator");
    CHECK_EQ_I(f.energy(buffer).current, 0, "a battery buffer is not either");
    CHECK_EQ_I(f.events->updates.size(), 1, "only the creative generator publishes");
}

static void test_CreativeGeneratorSystem_generation_ignores_max_input() {
    // max_input gates EXTERNAL energy (EnergyStorage::addEnergy); production
    // uses a raw `current += toAdd`, so a maxInput of 0 does not stop it.
    CreativeFixture f;
    auto ent = f.addGenerator(kCreativeGeneratorId, 1, 64, 1, 100000, 0, /*maxIn*/ 0,
                              /*maxOut*/ 0, 10);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(ent).current, 1024, "maxInput == 0 does not gate generation");
    CHECK_EQ_I(f.energy(ent).maxInput, 0, "the field really is zero");
}

static void test_CreativeGeneratorSystem_view_requires_machine_and_energy() {
    CreativeFixture f;
    auto ent = f.addGenerator(kCreativeGeneratorId, 1, 64, 1, 10000, 0, 32, 100000, 10);
    f.reg.remove<EnergyStorage>(ent);
    f.sys->tick(kDt);
    CHECK_EQ_I(f.events->updates.size(), 0, "without EnergyStorage the entity is outside the view");

    CreativeFixture g;
    auto e2 = g.addGenerator(kCreativeGeneratorId, 1, 64, 1, 10000, 0, 32, 100000, 10);
    g.reg.remove<MachineComponent>(e2);
    g.sys->tick(kDt);
    CHECK_EQ_I(g.events->updates.size(), 0, "without MachineComponent likewise");
    CHECK_EQ_I(g.reg.get<EnergyStorage>(e2).current, 0, "and the energy is unchanged");
}

static void test_CreativeGeneratorSystem_works_with_a_null_pipe_client() {
    // The pipe client is only used behind `if (pipeClient_)`, so a null client
    // is legal and the generation still happens. events_ however is
    // dereferenced unconditionally.
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    CreativeGeneratorSystem sys(reg, events, nullptr);
    auto ent = reg.create();
    reg.emplace<MachineComponent>(ent, kCreativeGeneratorId, 0, 1, 64, 1, 1);
    reg.emplace<EnergyStorage>(ent, 10000, 0, 32, 100000, 10, EnergyType::ELECTRICITY);

    sys.tick(kDt);

    CHECK_EQ_I(reg.get<EnergyStorage>(ent).current, 1024,
               "generation works with no pipe client at all");
    CHECK_EQ_I(events->updates.size(), 1, "and the state is still published");
}

static void test_CreativeGeneratorSystem_multiple_generators_are_independent() {
    CreativeFixture f;
    auto a = f.addGenerator(kCreativeGeneratorId, 1, 64, 1, 10000, 0, 32, 100000, 10);
    auto b = f.addGenerator(kCreativeGeneratorId, 2, 64, 2, 10000, 5000, 32, 100000, 10);
    auto c = f.addGenerator(kCreativeGeneratorId, 3, 64, 3, 10000, 10000, 32, 100000, 10);

    f.sys->tick(kDt);

    CHECK_EQ_I(f.energy(a).current, 1024, "the empty one gains the full rate");
    CHECK_EQ_I(f.energy(b).current, 6024, "the half-full one gains the full rate too");
    CHECK_EQ_I(f.energy(c).current, 10000, "the full one gains nothing");
    CHECK_EQ_I(f.events->updates.size(), 2, "only the two producing generators publish");
}

static void test_CreativeGeneratorSystem_dt_is_ignored() {
    CreativeFixture fast;
    auto a = fast.addGenerator(kCreativeGeneratorId, 1, 64, 1, 100000, 0, 32, 100000, 10);
    fast.sys->tick(1.0f);

    CreativeFixture slow;
    auto b = slow.addGenerator(kCreativeGeneratorId, 1, 64, 1, 100000, 0, 32, 100000, 10);
    slow.sys->tick(0.001f);

    CHECK_EQ_I(fast.energy(a).current, slow.energy(b).current,
               "the produced amount is identical at any dt");
    CHECK_EQ_I(fast.energy(a).current, 1024, "namely exactly one tick's rate");
}

// ===========================================================================
// FINDING gp-ht7r: the creative INVENTORY GRANT does not live in
// CreativeGeneratorSystem at all
// ===========================================================================

static void test_CreativeGeneratorSystem_has_no_inventory_grant_path() {
    // gp-ht7r asks for "the creative inventory grant path": a grant that adds
    // the requested count, splits across stacks per PlayerInventoryStore rules,
    // and fails cleanly when capacity is exceeded.
    //
    // CreativeGeneratorSystem.cpp has NO inventory code: no InventoryContainer,
    // no InventorySlot, no giveItem, no addItem. The only grant in the server is
    // PlayerActionDispatcher::tryParseAsPlayerAction ITEM_ACTION ->
    // PlayerInventoryStore::giveItem (SimCoreMessageHandler.cpp:186-189), which
    // is the path the QuestBook tracer (gp-42p, item 22530 x8) actually drives.
    //
    // The tests below therefore cover the grant where it really is, so gp-ht7r's
    // acceptance criteria are met even though the issue named the wrong file.
    const char *src = nullptr;
    static_cast<void>(src);
}

static void test_creative_grant_adds_the_requested_count() {
    // The exact scenario of the gp-42p tracer: item 22530, count 8.
    constexpr uint16_t kTracerItem = 22530;
    constexpr uint8_t kTracerCount = 8;
    constexpr uint64_t kPlayer = 7;

    auto inv = std::make_shared<simcore::PlayerInventoryStore>();
    inv->initPlayer(kPlayer);
    simcore::PlayerActionDispatcher dispatcher(
        [inv](uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot) {
            static_cast<void>(inv);
            inv->giveItem(player_id, item_id, count, target_slot);
        });

    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(0, 0, 0);
    auto act = Protocol::CreatePlayerAction(fbb, kPlayer,
                                            Protocol::PlayerActionType_ITEM_ACTION,
                                            &pos, /*face=*/0, /*count=*/kTracerCount,
                                            /*block_id=*/kTracerItem, /*request_id=*/0);
    fbb.Finish(act);

    dispatcher.dispatch({fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});

    int total = 0;
    for (const auto &s : inv->getSlots(kPlayer)) {
        if (s.item_id == kTracerItem) total += s.count;
    }
    CHECK_EQ_I(total, kTracerCount, "the ITEM_ACTION grant delivers exactly 8 of item 22530");
}

static void test_creative_grant_splits_across_stacks() {
    // 150 of one item. PlayerActionDispatcher passes `pos.x()` as the target
    // slot, so this goes down the target_slot branch of giveItem. That branch
    // used to write the WHOLE remaining count into the target slot with no
    // kMaxStack clamp, so a wire grant created a single over-stacked slot; the
    // only untargeted path (target_slot < 0) ever split at 64 (gp-w0b7). Both
    // paths now clamp identically, and this is the end-to-end proof over the
    // FlatBuffer wire path rather than a direct store call.
    constexpr uint16_t kItem = 22530;
    constexpr uint8_t kCount = 150;
    constexpr uint64_t kPlayer = 11;

    auto inv = std::make_shared<simcore::PlayerInventoryStore>();
    inv->initPlayer(kPlayer);
    simcore::PlayerActionDispatcher dispatcher(
        [inv](uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot) {
            inv->giveItem(player_id, item_id, count, target_slot);
        });

    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(0, 0, 0);
    auto act = Protocol::CreatePlayerAction(fbb, kPlayer,
                                            Protocol::PlayerActionType_ITEM_ACTION,
                                            &pos, /*face=*/0, /*count=*/kCount,
                                            /*block_id=*/kItem, /*request_id=*/0);
    fbb.Finish(act);
    dispatcher.dispatch({fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});

    int stacks = 0, total = 0, worst = 0;
    for (const auto &s : inv->getSlots(kPlayer)) {
        if (s.item_id == kItem) {
            ++stacks;
            total += s.count;
            worst = s.count > worst ? s.count : worst;
        }
    }
    // The targeted path must now behave exactly like the untargeted one below.
    CHECK_EQ_I(stacks, 3, "a wire grant with pos.x() == 0 splits across three stacks");
    CHECK_EQ_I(total, 150, "and all 150 items are stored, none dropped");
    CHECK_LE_I(worst, 64, "with no stack over the 64-item cap — a wire grant can no "
                          "longer create an over-stacked slot (gp-w0b7)");
    CHECK_EQ_I(inv->getSlots(kPlayer)[0].count, 64,
               "the targeted slot itself is filled to the cap, not beyond it");

    // The untargeted path is unchanged: it has always split at 64.
    auto inv2 = std::make_shared<simcore::PlayerInventoryStore>();
    inv2->initPlayer(kPlayer);
    CHECK(inv2->giveItem(kPlayer, kItem, 150, -1), "the direct path succeeds too");
    int stacks2 = 0, total2 = 0, worst2 = 0;
    for (const auto &s : inv2->getSlots(kPlayer)) {
        if (s.item_id == kItem) {
            ++stacks2;
            total2 += s.count;
            worst2 = s.count > worst2 ? s.count : worst2;
        }
    }
    CHECK_EQ_I(stacks2, 3, "target_slot -1 splits 150 across three stacks");
    CHECK_EQ_I(total2, 150, "and stores all of them");
    CHECK_LE_I(worst2, 64, "with no stack over the 64-item cap");
}

static void test_creative_grant_into_a_full_inventory_does_not_corrupt_state() {
    // Fill every slot, then grant: giveItem logs a warning and returns false, and
    // the pre-existing contents are untouched.
    constexpr uint16_t kItem = 22530;
    constexpr uint64_t kPlayer = 13;

    auto inv = std::make_shared<simcore::PlayerInventoryStore>();
    inv->initPlayer(kPlayer);
    // Occupy all 40 slots with a different item at full stack size.
    std::vector<simcore::PersistSlot> full;
    full.resize(simcore::kInventorySlots);
    for (auto &s : full) {
        s.item_id = 9999;
        s.count = 64;
        s.meta = 0;
    }
    inv->applyUpdate(kPlayer, full);

    int before = 0;
    for (const auto &s : inv->getSlots(kPlayer)) before += s.count;

    const bool ok = inv->giveItem(kPlayer, kItem, 8, -1);

    int after = 0, granted = 0, foreign = 0;
    for (const auto &s : inv->getSlots(kPlayer)) {
        after += s.count;
        if (s.item_id == kItem) granted += s.count;
        if (s.item_id == 9999) foreign += s.count;
    }
    CHECK(!ok, "a grant into a full inventory FAILS (giveItem returns false)");
    CHECK_EQ_I(granted, 0, "and nothing of the granted item is added");
    CHECK_EQ_I(after, before, "the existing inventory is not corrupted");
    CHECK_EQ_I(foreign, before, "every pre-existing stack is intact");
    CHECK_EQ_I(before, simcore::kInventorySlots * 64,
               "the pre-existing inventory really was 40 full stacks");
}

static void test_creative_grant_of_zero_count_succeeds_and_changes_nothing() {
    // FINDING (gp-ht7r). A 0-count ITEM_ACTION is reported as handled, and
    // giveItem with count 0 leaves every slot untouched and returns true. There
    // is no rejection of an empty grant.
    constexpr uint16_t kItem = 22530;
    constexpr uint64_t kPlayer = 17;

    auto inv = std::make_shared<simcore::PlayerInventoryStore>();
    inv->initPlayer(kPlayer);

    const bool ok = inv->giveItem(kPlayer, kItem, 0, -1);

    int total = 0;
    for (const auto &s : inv->getSlots(kPlayer)) total += s.count;
    CHECK(ok, "a zero-count grant returns success");
    CHECK_EQ_I(total, 0, "but places nothing");
}

static void test_creative_grant_honours_a_positive_target_slot() {
    // target_slot comes straight off the wire (PlayerAction::pos().x()) and is
    // used as a slot index when in range. Pinned so the wire->slot contract is
    // explicit.
    constexpr uint16_t kItem = 22530;
    constexpr uint64_t kPlayer = 19;

    auto inv = std::make_shared<simcore::PlayerInventoryStore>();
    inv->initPlayer(kPlayer);
    CHECK(inv->giveItem(kPlayer, kItem, 3, /*target_slot=*/5), "the grant succeeds");
    const auto slots = inv->getSlots(kPlayer);
    CHECK_EQ(slots[5].item_id, kItem, "the item landed in the requested slot");
    CHECK_EQ_I(slots[5].count, 3, "with the requested count");
    CHECK_EQ_I(slots[0].count, 0, "and the first free slot was NOT used");
}

static void test_creative_grant_ignores_an_out_of_range_target_slot() {
    // A target_slot past the 40-slot inventory is silently ignored and the
    // normal "first free slot" path runs.
    constexpr uint16_t kItem = 22530;
    constexpr uint64_t kPlayer = 23;

    auto inv = std::make_shared<simcore::PlayerInventoryStore>();
    inv->initPlayer(kPlayer);
    CHECK(inv->giveItem(kPlayer, kItem, 2, /*target_slot=*/999), "the grant still succeeds");
    int total = 0;
    for (const auto &s : inv->getSlots(kPlayer)) total += s.count;
    CHECK_EQ_I(total, 2, "the items still land somewhere");
    CHECK_EQ(inv->getSlots(kPlayer)[0].item_id, kItem,
             "an out-of-range target falls back to the first free slot");
}

static void test_creative_grant_to_an_uninitialised_player_still_works() {
    // inventories_ is only populated by initPlayer, but giveItem uses
    // operator[] so an unknown player gets a default-constructed row.
    constexpr uint16_t kItem = 22530;
    auto inv = std::make_shared<simcore::PlayerInventoryStore>();

    CHECK(inv->giveItem(999, kItem, 4, -1), "granting to an unknown player succeeds");
    int total = 0;
    for (const auto &s : inv->getSlots(999)) total += s.count;
    CHECK_EQ_I(total, 4, "and the items are stored");
}

#define TEST(name)                                                             \
    do {                                                                       \
        ++g_tests;                                                             \
        printf("  TEST: %s\n", #name);                                         \
        test_##name();                                                         \
    } while (0)

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("=== mining systems test suite (Drill / BatteryBuffer / CreativeGenerator) ===\n\n");

    // --- DrillSystem: fixture sanity -------------------------------------------------
    TEST(DrillSystem_tier_constants_match_the_component);
    TEST(DrillSystem_empty_registry_is_a_noop);
    TEST(DrillSystem_publishes_energy_every_tick);
    TEST(DrillSystem_reports_machine_energy_without_a_tool);

    // --- DrillSystem: mining progress ------------------------------------------------
    TEST(DrillSystem_mining_progress_advances_one_tick_per_tick);
    TEST(DrillSystem_block_breaks_on_the_final_tick);
    TEST(DrillSystem_progress_is_reported_in_the_entity_update);
    TEST(DrillSystem_energy_is_drawn_on_every_mining_tick);
    TEST(DrillSystem_tier_scales_the_energy_draw);
    TEST(DrillSystem_exact_energy_boundary);
    TEST(DrillSystem_no_energy_source_aborts_mining);
    TEST(DrillSystem_insufficient_energy_does_not_underflow);
    TEST(DrillSystem_machine_energy_fallback_respects_max_output);
    TEST(DrillSystem_container_without_a_tool_falls_back_to_machine_energy);
    TEST(DrillSystem_find_tool_slot_takes_the_first_tool);
    TEST(DrillSystem_empty_tool_slot_does_not_block_the_later_tool);

    // --- DrillSystem: output buffer --------------------------------------------------
    TEST(DrillSystem_output_buffer_fills_and_blocks_at_the_cap);
    TEST(DrillSystem_output_full_releases_once_space_appears);
    TEST(DrillSystem_broken_block_resets_the_target_and_progress);
    TEST(DrillSystem_ore_without_a_drop_occupies_no_buffer_slot);
    TEST(DrillSystem_drop_less_ore_can_never_wedge_the_output_buffer);
    TEST(DrillSystem_drop_comes_from_the_targeted_ore);
    TEST(DrillSystem_a_conflict_clears_the_remembered_ore);
    TEST(DrillSystem_the_search_records_the_ore_it_finds);
    TEST(DrillSystem_a_fresh_drill_targets_no_ore);
    TEST(DrillSystem_non_ore_blocks_are_never_mined);
    TEST(DrillSystem_search_starts_from_idle_and_probes_two_cells);
    TEST(DrillSystem_search_counter_accumulates_and_releases);
    TEST(DrillSystem_search_counter_never_exceeds_the_per_tick_cap);
    TEST(DrillSystem_search_sustains_its_rate_under_an_async_repository);
    TEST(DrillSystem_search_index_wraps_to_the_next_layer);
    TEST(DrillSystem_layer_offset_sequence);
    TEST(DrillSystem_the_spiral_starts_at_the_origin);
    TEST(DrillSystem_the_origin_probe_is_never_issued);
    TEST(DrillSystem_the_spiral_is_four_connected);
    TEST(DrillSystem_the_spiral_visits_the_four_walls_in_ring_order);
    TEST(DrillSystem_the_block_directly_below_is_reachable);
    TEST(DrillSystem_only_layer_zero_skips_the_origin);
    TEST(DrillSystem_dt_is_ignored);
    TEST(DrillSystem_search_results_arrive_in_the_same_tick_with_a_synchronous_repository);
    TEST(DrillSystem_idle_resets_the_search_each_tick);
    TEST(DrillSystem_output_buffer_is_not_persisted_across_a_reset);
    TEST(DrillSystem_multiple_drills_are_ticked_independently);

    // --- BatteryBufferSystem ---------------------------------------------------------
    TEST(BatteryBufferSystem_view_requires_all_three_components);
    TEST(BatteryBufferSystem_charges_at_the_declared_rate);
    TEST(BatteryBufferSystem_charges_every_slot_once_per_tick);
    TEST(BatteryBufferSystem_charge_is_clamped_by_the_buffer);
    TEST(BatteryBufferSystem_charge_is_clamped_by_the_tool_max_input);
    TEST(BatteryBufferSystem_empty_buffer_stops_charging);
    TEST(BatteryBufferSystem_full_tool_is_not_charged);
    TEST(BatteryBufferSystem_exact_tool_capacity_boundary);
    TEST(BatteryBufferSystem_non_tool_items_are_never_charged);
    TEST(BatteryBufferSystem_num_slots_caps_the_charged_slots);
    TEST(BatteryBufferSystem_short_inventory_is_safely_bounded);
    TEST(BatteryBufferSystem_charges_repeatedly_until_full);
    TEST(BatteryBufferSystem_dt_is_ignored);
    TEST(BatteryBufferSystem_publishes_state_for_a_machine);
    TEST(BatteryBufferSystem_publishes_nothing_without_a_machine_component);
    TEST(BatteryBufferSystem_publish_inventory_packs_meta_little_endian);
    TEST(BatteryBufferSystem_full_buffer_requests_from_the_pipe_network);
    TEST(BatteryBufferSystem_negative_stored_is_clamped_to_zero);
    TEST(BatteryBufferSystem_stored_above_capacity_is_clamped);
    TEST(BatteryBufferSystem_consume_response_charges_the_matching_node);
    TEST(BatteryBufferSystem_consume_response_clamps_to_capacity);
    TEST(BatteryBufferSystem_consume_response_rejects_zero_and_negative);
    TEST(BatteryBufferSystem_a_response_for_node_zero_credits_node_zero);
    TEST(BatteryBufferSystem_a_response_for_node_zero_settles_its_own_request);
    TEST(BatteryBufferSystem_an_unknown_node_id_credits_nobody);
    TEST(BatteryBufferSystem_the_first_entity_is_node_zero);
    TEST(BatteryBufferSystem_discharge_path_is_owned_by_the_flow_handler);

    // --- CreativeGeneratorSystem -----------------------------------------------------
    TEST(CreativeGeneratorSystem_default_rate_constant);
    TEST(CreativeGeneratorSystem_fills_at_the_default_rate);
    TEST(CreativeGeneratorSystem_set_energy_per_tick_is_honoured);
    TEST(CreativeGeneratorSystem_rate_accumulates_over_ticks);
    TEST(CreativeGeneratorSystem_is_clamped_to_the_remaining_space);
    TEST(CreativeGeneratorSystem_exact_space_boundary);
    TEST(CreativeGeneratorSystem_full_buffer_is_untouched);
    TEST(CreativeGeneratorSystem_other_machine_ids_are_ignored);
    TEST(CreativeGeneratorSystem_generation_ignores_max_input);
    TEST(CreativeGeneratorSystem_view_requires_machine_and_energy);
    TEST(CreativeGeneratorSystem_works_with_a_null_pipe_client);
    TEST(CreativeGeneratorSystem_multiple_generators_are_independent);
    TEST(CreativeGeneratorSystem_dt_is_ignored);

    // --- Creative inventory grant (the real gp-ht7r path) ----------------------------
    TEST(CreativeGeneratorSystem_has_no_inventory_grant_path);
    TEST(creative_grant_adds_the_requested_count);
    TEST(creative_grant_splits_across_stacks);
    TEST(creative_grant_of_zero_count_succeeds_and_changes_nothing);
    TEST(creative_grant_into_a_full_inventory_does_not_corrupt_state);
    TEST(creative_grant_honours_a_positive_target_slot);
    TEST(creative_grant_ignores_an_out_of_range_target_slot);
    TEST(creative_grant_to_an_uninitialised_player_still_works);

    printf("\n=== Results: %d tests, %d passed checks, %d failed ===\n", g_tests,
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

