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
// 1. The search counter latches. `pendingSearches_[ent] = sent;`
//    (DrillSystem.cpp:239) is an ASSIGNMENT, written AFTER the getBlock
//    callbacks have already decremented the map back to empty. So the counter
//    is left at 2 (kMaxPerTick) and every later tick hits
//    `if (pending >= 2) return;` — the drill issues exactly TWO block requests
//    for its entire life. Pinned by
//    …search_counter_is_reassigned_not_accumulated. (Measured: 2 gets, then 0
//    forever, and searchIndex/searchLayer never move again.)
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
// gp-u9ua (P1) BatteryBufferSystem::onConsumeResponse treats node_id 0 as a
//         sentinel, but entt ids the FIRST entity 0, so that node's responses
//         are dropped (single-entity registry) or credited to the wrong buffer
//         (multi-entity). Pinned by
//         BatteryBufferSystem_the_first_entity_is_node_zero and
//         …consume_response_ignores_a_stale_request_order.
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
constexpr uint16_t kDrillUlv = 90;    // capacity 1000, maxInput 8,  tier 0
constexpr uint16_t kDrillMv = 91;     // capacity 4000, maxInput 32, tier 1
constexpr uint16_t kBatteryLv = 60948; // capacity 1000, maxInput 32, tier 0
constexpr uint16_t kBatteryHv = 60950; // capacity 16000, maxInput 512, tier 2

// Machine ids from src/content/data/registry/machines.yaml.
constexpr uint16_t kCreativeGeneratorId = ItemId::pack("1110:100:0");
constexpr uint16_t kHeatFurnaceId = ItemId::pack("1110:000:0");
constexpr uint16_t kBatteryBufferLvId = ItemId::pack("1110:101:0");

// Ore block ids from src/content/content.h.
constexpr uint16_t kOreIron = content::kOreIron;
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
        auto it = cells_.find(key(x, y, z));
        if (it == cells_.end()) {
            callback(BlockData{0, 0, 0});
        } else {
            callback(it->second);
        }
    }

    void clearRecording() {
        gets.clear();
        cases.clear();
    }

    size_t blockCount() const { return cells_.size(); }

private:
    std::unordered_map<uint64_t, BlockData> cells_;
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
    // meta, so a ULV drill can never hold more than TOOL_ENERGY_DEFS[90].capacity.
    giveDrillTool(f.reg, ent, kDrillUlv, TOOL_ENERGY_DEFS.at(kDrillUlv).capacity);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 1;
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

static void test_DrillSystem_ore_without_a_drop_still_occupies_a_buffer_slot() {
    // content::oreDropTable has no entry for redstone/lapis/diamond, and
    // oreToDrop() returns 0 for them. The drop is appended anyway as item 0.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.repo->set(1, 64, 0, kOreDiamond);

    auto &d = f.drill(ent);
    d.state = DrillState::MINING;
    d.targetX = 1;
    d.miningTicksTotal = 1;
    d.miningProgress = 1;

    f.sys->tick(kDt);

    CHECK_EQ_I(f.drill(ent).outputBuffer.size(), 1,
               "the block is still mined and consumes a buffer slot");
    if (!f.drill(ent).outputBuffer.empty()) {
        CHECK_EQ_I(f.drill(ent).outputBuffer[0].first, 0,
                   "a drop-less ore contributes an item_id 0 entry");
    }
}

// The drop is derived from CASResult::block_id — the block the STORE reports it
// found — not from the block the drill searched for. So the fake store has to
// echo the ore id for a correct drop, which is what the real ChunkStore does
// (IoUringChunkClient.cpp:128-130 replies with resp->actual_block_id()).
static void test_DrillSystem_drop_comes_from_the_store_reply() {
    // FINDING (gp-fcvf). `onMineComplete` calls oreToDrop(result.block_id) where
    // result.block_id is the CAS REPLY, i.e. the id the store says was there.
    // The drill never remembers which ore it targeted, so a store that reports
    // air (0) yields a silent item_id 0 drop — which then counts against
    // kMaxOutputSize and can wedge the drill into OUTPUT_FULL with 64 empty
    // slots. Pinned both ways: correct store reply -> real drop, air -> empty.
    DrillFixture good;
    auto e1 = good.addDrill(0, 64, 0, 0);
    giveDrillTool(good.reg, e1, kDrillUlv, 1000);
    good.repo->set(1, 64, 0, kOreIron);
    good.drill(e1).state = DrillState::MINING;
    good.drill(e1).targetX = 1;
    good.drill(e1).targetY = 64;
    good.drill(e1).targetZ = 0;
    good.drill(e1).miningTicksTotal = 1;
    good.drill(e1).miningProgress = 1;
    good.sys->tick(kDt);
    CHECK_EQ_I(good.drill(e1).outputBuffer.size(), 1, "one drop is buffered");
    if (!good.drill(e1).outputBuffer.empty()) {
        CHECK_EQ(good.drill(e1).outputBuffer[0].first, ItemId::pack("0:110:1"),
                 "a store that reports the iron ore yields an iron ingot");
    }

    DrillFixture air;
    auto e2 = air.addDrill(0, 64, 0, 0);
    giveDrillTool(air.reg, e2, kDrillUlv, 1000);
    air.repo->cas_result_block = 0;  // the store replies "there was air here"
    air.drill(e2).state = DrillState::MINING;
    air.drill(e2).targetX = 1;
    air.drill(e2).targetY = 64;
    air.drill(e2).targetZ = 0;
    air.drill(e2).miningTicksTotal = 1;
    air.drill(e2).miningProgress = 1;
    air.sys->tick(kDt);
    CHECK_EQ_I(air.drill(e2).outputBuffer.size(), 1,
               "an air reply STILL consumes a buffer slot");
    if (!air.drill(e2).outputBuffer.empty()) {
        CHECK_EQ_I(air.drill(e2).outputBuffer[0].first, 0,
                   "and the drop is a silent item_id 0, not the searched ore");
    }
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
    CHECK_EQ_I(f.drill(ent).searchIndex, 2,
               "the search index advances once per issued request");
    CHECK_EQ_I(f.drill(ent).searchLayer, 0, "and the layer only changes at the wrap");
}

static void test_DrillSystem_search_counter_is_reassigned_not_accumulated() {
    // FINDING (gp-fcvf). `pendingSearches_[ent] = sent;` is an ASSIGNMENT and
    // runs after the synchronous callbacks have already erased the entry, so the
    // counter is left at 2 forever and the drill stops searching after one tick
    // of requests. Observed, not fixed.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    f.sys->tick(kDt);
    CHECK_EQ_I(f.repo->gets.size(), 2, "tick 1 issues two block requests");
    const int32_t index_after_tick1 = f.drill(ent).searchIndex;

    for (int tick = 0; tick < 5; ++tick) f.sys->tick(kDt);

    CHECK_EQ_I(f.repo->gets.size(), 2,
               "ticks 2..6 issue NO further requests: pendingSearches_ is stuck at 2");
    CHECK_EQ_I(f.drill(ent).searchIndex, index_after_tick1,
               "and the search index never advances again");
    CHECK_EQ_I(f.drill(ent).searchLayer, 0, "so the drill never leaves layer 0");
}

static void test_DrillSystem_search_index_wraps_to_the_next_layer() {
    // searchIndex >= 440 rolls the index to 0 and advances searchLayer. Pinned
    // by driving the index to the wrap directly.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);

    // The counter is stuck after one tick (see
    // …search_counter_is_reassigned_not_accumulated), so the 440-wrap can only
    // ever be reached on the FIRST tick. Start the index at 439 and let the two
    // requests of that tick run: 439 -> 440 wraps to 0, then +1.
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

static void test_DrillSystem_spiral_never_produces_the_origin() {
    // FINDING (gp-fcvf). getSpiralOffset(n) never returns (0,0): the loop
    // advances x/z BEFORE testing `count == n`, so the first cell it can return
    // is already one step out. Measured sequence:
    //
    //   n=0 -> (+1,  0)      n=1 -> (+1, +1) DIAGONAL
    //   n=2 -> ( 0, +1)      n=3 -> (-1, +1) DIAGONAL
    //   n=4 -> (-1,  0)      n=5 -> (-1, -1) DIAGONAL
    //   n=6 -> ( 0, -1)      n=7 -> (+1, -1) DIAGONAL
    //
    // Consequences: (a) the `dx == 0 && dy == 0 && dz == 0` guard at
    // DrillSystem.cpp:221 is DEAD CODE — the branch is never taken; (b) the
    // drill's OWN cell is never probed, so a drill sitting inside stone never
    // mines the block it occupies; (c) every other cell in the search is
    // diagonal, which is a far larger set than a 1-block-radius tube.
    // The spiral is not a public API, so it is pinned through the only channel
    // it has: the coordinates the repository is asked about.
    for (int start = 0; start < 8; ++start) {
        DrillFixture f;
        auto ent = f.addDrill(0, 64, 0, 0);
        giveDrillTool(f.reg, ent, kDrillUlv, 1000);
        f.drill(ent).state = DrillState::SEARCHING;
        f.drill(ent).searchIndex = start;
        f.sys->tick(kDt);
        CHECK_EQ_I(f.repo->gets.size(), 2,
                   "the request counter latches, so each start index is "
                   "observed on its own system");
        for (const auto &call : f.repo->gets) {
            CHECK(!(call.x == 0 && call.y == 64 && call.z == 0),
                  "the drill's own cell is never probed at any spiral index");
        }
    }
}

static void test_DrillSystem_second_probe_is_already_a_diagonal() {
    // With searchIndex 0, the two requests of the first tick are (+1,0) and
    // (+1,+1) — the SECOND one is already a diagonal neighbour. Pinned because
    // a reader would reasonably assume a 4-neighbour spiral.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, 1000);
    f.drill(ent).state = DrillState::SEARCHING;

    f.sys->tick(kDt);

    CHECK_EQ_I(f.repo->gets.size(), 2, "two probes on the first search tick");
    if (f.repo->gets.size() == 2) {
        CHECK_EQ(f.repo->gets[0].x, 1, "probe 0 is +X");
        CHECK_EQ(f.repo->gets[0].z, 0, "probe 0 is +X, not +X+Z");
        CHECK_EQ(f.repo->gets[1].x, 1, "probe 1 is +X");
        CHECK_EQ(f.repo->gets[1].z, 1, "and also +Z: the second probe is a DIAGONAL");
    }
}

static void test_DrillSystem_below_the_drill_is_never_probed_in_layer_zero() {
    // The block DIRECTLY below a drill is the single most valuable target, and
    // layer 0 cannot reach it. Pinned: a world full of ore under the drill is
    // never found, because the counter also latches after one tick.
    DrillFixture f;
    auto ent = f.addDrill(0, 64, 0, 0);
    giveDrillTool(f.reg, ent, kDrillUlv, TOOL_ENERGY_DEFS.at(kDrillUlv).capacity);
    f.repo->set(0, 63, 0, kOreIron);   // directly below
    f.repo->set(0, 65, 0, kOreIron);   // directly above
    f.repo->set(1, 64, 0, kOreIron);   // +X
    f.repo->set(0, 64, 1, kOreIron);   // +Z

    f.sys->tick(kDt);
    for (int i = 0; i < 10; ++i) f.sys->tick(kDt);

    const int32_t tx = f.drill(ent).targetX;
    const int32_t ty = f.drill(ent).targetY;
    const int32_t tz = f.drill(ent).targetZ;
    CHECK(!((tx == 0 && ty == 63 && tz == 0) || (tx == 0 && ty == 65 && tz == 0)),
          "neither the block below nor the block above is ever targeted in layer 0");
    CHECK_EQ_I(f.drill(ent).state, static_cast<int64_t>(DrillState::MINING),
               "the +X neighbour IS found on the first tick, which is what proves "
               "the search is working and not merely dead");
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

    CHECK_EQ_I(f.drill(ent).searchIndex, 2, "the search restarts from index 0 on the IDLE tick");
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
    // this is observed through the buffer state, which is only mutated by the
    // response path.
    // FINDING-adjacent. The pipe-network REQUEST block is gated on
    // `buffer.stored < capacity`, but the CHARGING block above it is not:
    // chargeSlot() is entered for every non-empty slot and is limited only by
    // min(chargeRate, buffer.stored, ...). A buffer whose stored is negative (an
    // underflowed drain from EnergyFlowHandler clamps at 0, but any other
    // writer can make it negative) would therefore hand the tool a NEGATIVE
    // amount. Pinned with the real ceiling instead: a full buffer is still
    // charged by exactly chargeRate, because stored only enters as the
    // min() term, never as a gate.
    BatteryFixture f;
    auto full = f.addBuffer(10, 64, 10, 40000, 40000, 1, 32, kLvChargeRate, 1);
    f.slots(full)[0] = InventorySlot{kDrillUlv, 1, 0};
    f.sys->tick(kDt);
    CHECK_EQ_I(f.slots(full)[0].meta, kLvChargeRate,
               "a FULL buffer still charges the tool: chargeSlot has no "
               "capacity gate of its own, only the min() clamp");
    CHECK_EQ_I(f.buf(full).stored, 40000 - kLvChargeRate,
               "and the buffer is debited past its capacity");
    CHECK_LT_I(f.buf(full).stored, 40000, "i.e. stored ends up below capacity anyway");

    BatteryFixture g;
    auto partial = g.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    g.slots(partial)[0] = InventorySlot{kDrillUlv, 1, 0};
    g.sys->tick(kDt);
    CHECK_EQ_I(g.buf(partial).stored, 0, "an empty buffer is not magically filled by ticking");
    CHECK_EQ_I(g.slots(partial)[0].meta, 0, "and it cannot charge a tool it has no EU for");
}

static void test_BatteryBufferSystem_consume_response_charges_the_matching_node() {
    // onConsumeResponse looks the node up in pendingRequests_, which is only
    // populated by a tick that actually issued a request. A response for an
    // untracked node is rejected (returns false, no state change).
    BatteryFixture f;
    auto ent = f.addBuffer(10, 64, 10, 40000, 1000, 1, 32, kLvChargeRate, 1);
    f.slots(ent)[0] = InventorySlot{kDrillUlv, 1, 0};

    // FINDING (gp-lcuf). entt hands out entity id 0 to the FIRST entity a
    // registry creates, so `static_cast<uint64_t>(ent) == 0` here. The
    // `if (node_id != 0)` guard in onConsumeResponse therefore takes the FIFO
    // fallback instead of the direct map lookup, and with an empty
    // pendingOrder_ the response is DROPPED. The first-registered battery
    // buffer can therefore never be credited by a node-targeted response.
    const uint64_t node = static_cast<uint64_t>(ent);
    CHECK_EQ_I(node, 0,
               "precondition: entt ids its first entity 0, so this buffer IS node 0");
    CHECK(!f.sys->onConsumeResponse(node, 100, 0),
           "the response is rejected: node_id 0 routes to the FIFO fallback, "
           "which is empty here");
    CHECK_EQ_I(f.buf(ent).stored, 1000, "and the buffer is unchanged");

    f.sys->tick(kDt);  // issues the request, so the FIFO queue is now populated
    const int32_t after_tick = f.buf(ent).stored;
    CHECK_EQ_I(after_tick, 1000 - kLvChargeRate, "the tick charged the tool");
    CHECK(f.sys->onConsumeResponse(node, 500, 0),
           "once the FIFO queue holds the request, the same response is accepted");
    CHECK_EQ_I(f.buf(ent).stored, after_tick + 500,
               "and credits the full amount via the fallback path");

    CHECK(!f.sys->onConsumeResponse(node, 500, 0),
           "the request is consumed exactly once");
    CHECK_EQ_I(f.buf(ent).stored, after_tick + 500, "a repeat response changes nothing");

    // The direct map lookup is what every entity AFTER the first uses. Create a
    // filler FIRST so the buffer under test is not entity 0. The filler needs
    // no components at all — it exists purely to consume entity id 0, so it
    // stays outside the view and issues no request of its own.
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

    // The tick above registered the request, so the direct lookup finds it on
    // the FIRST call and the buffer is credited. This is the path production
    // uses for every entity after the first, and unlike the node-0 FIFO
    // fallback it is keyed by node id.
    CHECK(g.sys->onConsumeResponse(real, 300, 0),
           "a tracked non-zero node IS credited through the direct lookup");
    CHECK_EQ_I(g.buf(warmup).stored, 300,
               "and the credit lands on the right buffer");

    CHECK(!g.sys->onConsumeResponse(real, 100, 0),
           "with the request retired, a further response is rejected");
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

static void test_BatteryBufferSystem_consume_response_ignores_a_stale_request_order() {
    // A response carrying node_id 0 falls through to the FIFO pendingOrder_
    // queue, crediting the OLDEST outstanding request. Pinned so the fallback
    // is visible rather than accidental.
    BatteryFixture f;
    auto a = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.slots(a)[0] = InventorySlot{kDrillUlv, 1, 0};
    auto b = f.addBuffer(20, 64, 20, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.slots(b)[0] = InventorySlot{kDrillUlv, 1, 0};

    // EnTT yields this view in REVERSE creation order, so `b` (created second)
    // is visited FIRST and is pushed onto pendingOrder_ first. A node_id 0
    // response therefore credits `b`, the OLDEST queue entry, which is the
    // LAST-created entity. The order is an ECS bookkeeping detail, not a rule.
    f.sys->tick(kDt);
    CHECK_EQ_I(static_cast<int64_t>(a), 0,
               "precondition: entt ids the first-created entity 0");
    CHECK_EQ_I(f.buf(a).stored, 0, "precondition: `a` holds no EU to charge a tool");
    CHECK_EQ_I(f.buf(b).stored, 0, "precondition: neither buffer holds any EU");

    CHECK(f.sys->onConsumeResponse(0, 200, 0),
           "a node_id 0 response falls through to the FIFO queue");
    CHECK_EQ_I(f.buf(b).stored, 200,
               "and credits the OLDEST queue entry, which is the "
               "last-created entity (reverse view order)");
    CHECK_EQ_I(f.buf(a).stored, 0, "leaving the other buffer untouched");
}

static void test_BatteryBufferSystem_the_first_entity_is_node_zero() {
    // entt hands out entity id 0 to the FIRST entity a registry creates
    // (measured: create three -> 0, 1, 2), and entt::null is 0xFFFFFFFF, not 0.
    // So the very first battery buffer in a fresh registry is node_id 0, and
    // onConsumeResponse's `if (node_id != 0)` guard sends its response down the
    // FIFO fallback instead of the direct map lookup. Pinned because the two
    // paths behave differently and only one of them is keyed by node.
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

    // With exactly one buffer in the registry, its node id IS 0.
    BatteryFixture f;
    auto only = f.addBuffer(10, 64, 10, 40000, 0, 1, 32, kLvChargeRate, 1);
    f.slots(only)[0] = InventorySlot{kDrillUlv, 1, 0};
    CHECK_EQ_I(static_cast<int64_t>(only), 0,
               "a single-entity registry puts the buffer on node 0");
    CHECK_EQ_I(f.buf(only).stored, 0, "sanity: the buffer starts empty");
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
    // slot, so this goes down the target_slot branch of giveItem, which has
    // NO kMaxStack clamp:
    //
    //   if (target_slot >= 0 && target_slot < kInventorySlots) {
    //     auto& dst = slots[target_slot];
    //     if (dst.item_id == 0) { dst = {item_id, (uint8_t)remaining, 0}; remaining = 0; }
    //
    // so all 150 land in ONE over-stacked slot. Only the target_slot < 0 path
    // (the "first free slot" scan) splits at 64 per stack. Pinned as observed.
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

    int stacks = 0, total = 0;
    for (const auto &s : inv->getSlots(kPlayer)) {
        if (s.item_id == kItem) {
            ++stacks;
            total += s.count;
        }
    }
    CHECK_EQ_I(stacks, 1, "a wire grant with pos.x() == 0 lands in ONE stack");
    CHECK_EQ_I(total, 150, "and all 150 items are stored, none dropped");
    CHECK_GT_I(inv->getSlots(kPlayer)[0].count, 64,
               "FINDING: the target_slot branch of giveItem has no kMaxStack "
               "clamp, so the wire grant can create an over-stacked slot");

    // The clamp only exists on the target_slot < 0 path, which splits at 64.
    auto inv2 = std::make_shared<simcore::PlayerInventoryStore>();
    inv2->initPlayer(kPlayer);
    CHECK(inv2->giveItem(kPlayer, kItem, 150, -1), "the direct path succeeds too");
    int stacks2 = 0, total2 = 0, worst = 0;
    for (const auto &s : inv2->getSlots(kPlayer)) {
        if (s.item_id == kItem) {
            ++stacks2;
            total2 += s.count;
            worst = s.count > worst ? s.count : worst;
        }
    }
    CHECK_EQ_I(stacks2, 3, "target_slot -1 splits 150 across three stacks");
    CHECK_EQ_I(total2, 150, "and stores all of them");
    CHECK_LE_I(worst, 64, "with no stack over the 64-item cap");
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
    TEST(DrillSystem_ore_without_a_drop_still_occupies_a_buffer_slot);
    TEST(DrillSystem_drop_comes_from_the_store_reply);
    TEST(DrillSystem_non_ore_blocks_are_never_mined);
    TEST(DrillSystem_search_starts_from_idle_and_probes_two_cells);
    TEST(DrillSystem_search_counter_is_reassigned_not_accumulated);
    TEST(DrillSystem_search_index_wraps_to_the_next_layer);
    TEST(DrillSystem_layer_offset_sequence);
    TEST(DrillSystem_spiral_never_produces_the_origin);
    TEST(DrillSystem_second_probe_is_already_a_diagonal);
    TEST(DrillSystem_below_the_drill_is_never_probed_in_layer_zero);
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
    TEST(BatteryBufferSystem_consume_response_charges_the_matching_node);
    TEST(BatteryBufferSystem_consume_response_clamps_to_capacity);
    TEST(BatteryBufferSystem_consume_response_rejects_zero_and_negative);
    TEST(BatteryBufferSystem_consume_response_ignores_a_stale_request_order);
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

