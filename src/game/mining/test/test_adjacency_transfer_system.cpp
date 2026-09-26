// AdjacencyTransferSystem unit tests (issue gp-epok).
//
// Covers src/game/mining/AdjacencyTransferSystem.cpp — the three-pass heat
// pipeline: adjacent heat transfer, overheat detection, environment cooling.
//
// ========================== PASS 2 GATE (gp-wjwr, now FIXED) ================
// Pass 2 (overheat detection) used to be DEAD AT RUNTIME. Its view was
//
//     reg_.view<HeatIntakeComponent, MultiblockController>()   // old line 118
//
// but MultiblockController is never emplaced into ANY entt::registry in src/.
// SimulationEngine keeps controllers in a plain container instead:
//
//     std::unordered_map<uint64_t, MultiblockController> controllers_;  // SimulationEngine.h:108
//     controllers_.emplace(id, MultiblockController(...));              // SimulationEngine.cpp:446
//
// `grep -rnE '(emplace|insert|assign|push)[^;]*<.*MultiblockController' src/`
// matches only test code. The block-entity creation path in
// SimulationEngine::onBlockChanged (SimulationEngine.cpp:178-283) emplaces
// Position, Block, MachineComponent, RecipeProgress, InventoryContainer,
// EnergyStorage and HeatIntakeComponent — never MultiblockController. The
// consequence was that pass 2 never iterated, so OverheatComponent was NEVER
// created by this system, and therefore ExplosionSystem (which requires
// OverheatComponent) could never fire either. The same defect existed in
// ExplosionSystem.cpp:13 and CoolantSystem.h:22.
//
// WHY THE GATE IS NOW `MachineComponent::mb_id != 0` AND NOT A NEW COMPONENT
// Emplacing a MultiblockController component would create a SECOND source of
// truth: EBFSystem / LCRSystem / LargeBoilerSystem all take
// `std::unordered_map<uint64_t, MultiblockController>&` and mutate it in
// place, SimulationEngine rewrites the hatches in place after registration
// (SimulationEngine.cpp:325-362), and removeBlockFromController erases from
// `it->second.blocks` (:57). An ECS mirror would be free to desync from all of
// that. `MachineComponent::mb_id` is already maintained by the engine and has
// no extra lifecycle to keep in sync:
//   * set to the controller id on formation   (SimulationEngine.cpp:309)
//   * refreshed on every block-change echo    (SimulationEngine.cpp:370)
//   * removed WITH the MachineComponent when the controller is destroyed
//     (destroyController, SimulationEngine.cpp:82)
// and only the anchor block is ever a machine (member blocks are casing/coil),
// so `mb_id != 0` is exactly "is a multiblock controller anchor".
//
// The tests below therefore build their subjects with a NONZERO mb_id, and
// pin the single-block (mb_id == 0) case as a negative.
//
// ============================ PASS 1 SEMANTICS ==============================
// View: reg_.view<MachineComponent, EnergyStorage, Position>()
//   * producers = HEAT machines where MachineRegistry::IsHeatSource(id)
//     (energy_out == HEAT) and current > 0 — snapshotted BEFORE any transfer
//   * sinks     = HEAT machines where IsHeatSink(id) (energy_in == HEAT)
//     and current < capacity
//   * transfer = min(capacity - current, producer.current), taken from the six
//     face neighbours in the fixed order +x, -x, +y, -y, +z, -z
//
// ========================= PASS 1 PROPAGATION (gp-c41q) ====================
// THE DECISION: heat propagates the FULL LENGTH of a chain in ONE tick, and
// does so INDEPENDENTLY of ECS creation order.
//
// The old pass iterated the sink loop in raw view order. EnTT yields that view
// in REVERSE creation order, so whether a hop completed in a tick was decided
// by which of two machines happened to be created last — an ECS bookkeeping
// detail, not a game rule. A three-block chain needed two ticks; a four-block
// chain three. The only reason a chain moved at all was that a relay already
// warm when the tick began could donate its pre-existing charge; a COLD relay
// is not in the producer snapshot at all, so a cold chain advanced exactly one
// block per tick and the drill heat (furnace) sat cold while charge piled up in
// the block in front of it.
//
// The fix orders the sink loop by DISTANCE FROM A PRODUCER rather than by
// creation order, which is a relaxation sweep: a relay at distance k is
// processed only after the producers at distance k-1 have refilled it, so it
// can pass their heat on to the distance k+1 block in the same tick. The
// nearest-source distance is computed with one BFS over the six-face adjacency
// graph, seeded at every source holding heat, and the sweep walks it in
// increasing distance with the view order as the tie-break (so two blocks at
// the same distance still resolve deterministically).
//
// The BFS expands ONLY through a node that is a heat SOURCE, because pass 1
// only ever moves heat out of such a node. A pure sink cannot relay.
//
// A sink with no adjacent heat source is unreachable by definition and is
// skipped, exactly as before — the BFS gives it no finite distance, so there is
// no behaviour change for an isolated sink.
//
// WHAT IS DELIBERATELY NOT CHANGED HERE (these are the issue's "related
// findings", already pinned as observed behaviour by this suite, and each is a
// separate design decision rather than a defect in the sweep):
//   * maxInput / maxOutput are still not consulted (transfer_ignores_…);
//   * adjacent source+sink pairs still ping-pong (…pair_drains_each_other);
//   * co-located producers still shadow by position (…shadow_each_other);
//   * pass 3 still force-overwrites EnergyStorage::current (…overwrites_…).
//
// Observed properties this suite pins (several are defects, asserted as-is):
//   1. maxInput / maxOutput are IGNORED. A transfer is never rate-limited; the
//      producer's entire content moves in one tick.
//   2. Transfer is FULL-CHAIN IN ONE TICK, in any creation order. Pinned by
//      …chain_propagates_the_whole_distance_in_one_tick,
//      …chain_order_does_not_change_how_far_heat_travels and
//      …four_block_chain_reaches_the_sink_in_one_tick.
//
// The system never publishes any event: `events_` is stored in the constructor
// and never used, so the tests pass an empty publisher.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

#include <entt/entt.hpp>

#include <engine/net/test/test.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/HeatIntakeComponent.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/Position.h>
#include <game/machines/HeatConstants.h>
#include <game/machines/OverheatComponent.h>
#include <game/mining/AdjacencyTransferSystem.h>

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has
// no GTest dependency, so this is the established convention for focused tests.

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}

// CHECK_EQ in the shared harness is a raw (a) == (b) compare; this wrapper
// normalises to int64_t so mixed width/signedness never trips -Wsign-compare.
#define CHECK_EQ_I(a, b, ...)                                                          \
    test_check(static_cast<int64_t>(a) == static_cast<int64_t>(b), __FILE__, __LINE__, \
               #a " == " #b, ##__VA_ARGS__)

namespace {

using simcore::Block;
using simcore::EnergyStorage;
using simcore::EnergyType;
using simcore::HeatIntakeComponent;
using simcore::MachineComponent;
using simcore::OverheatComponent;
using simcore::OverheatState;
using simcore::Position;

// Real ids from src/content/data/registry/machines.yaml.
constexpr uint16_t kHeatGenerator = ItemId::pack("1110:000:2"); // energy_out: HEAT
constexpr uint16_t kHeatFurnace = ItemId::pack("1110:000:0");   // energy_in:  HEAT
constexpr uint16_t kWaterBlock = ItemId::pack("0:0:9");

// A machine that is BOTH a heat source and a heat sink. None exists in
// machines.yaml, so it is registered at runtime through the public
// MachineRegistry::Register API and used as a relay in the chain tests.
constexpr uint16_t kHeatRelay = 0x7FFE;

// MachineRegistry has a private constructor, so LoadFromYaml is the only way to
// obtain one. A relay is added on top so the chain tests have a middle block
// that is both producer and consumer.
std::unique_ptr<MachineRegistry> makeRegistry() {
    auto reg = MachineRegistry::LoadFromYaml(GTNH_MACHINES_YAML);
    if (!reg) return nullptr;

    MachineInfo relay{};
    relay.id = kHeatRelay;
    relay.name = "heat_relay";
    relay.machine_class = "relay";
    relay.energy_in = EnergyType::HEAT;
    relay.energy_out = EnergyType::HEAT;
    relay.tier = 0;
    relay.capacity = 10000;
    relay.maxInput = 32;
    relay.maxOutput = 32;
    reg->Register(relay);
    return reg;
}

// The real machines.yaml is the single source of truth for IsHeatSource /
// IsHeatSink. Validated once up front: if the content data ever stops
// describing heat_generator as a source and heat_furnace as a sink, every
// transfer test below would silently become vacuous.
void checkRegistryPreconditions(const MachineRegistry& reg) {
    CHECK(reg.IsHeatSource(kHeatGenerator), "heat_generator is a heat SOURCE");
    CHECK(!reg.IsHeatSink(kHeatGenerator), "heat_generator is not a heat SINK");
    CHECK(reg.IsHeatSink(kHeatFurnace), "heat_furnace is a heat SINK");
    CHECK(!reg.IsHeatSource(kHeatFurnace), "heat_furnace is not a heat SOURCE");
    CHECK(reg.IsHeatSource(kHeatRelay) && reg.IsHeatSink(kHeatRelay),
          "the registered relay is both source and sink");
}

// A machine entity: MachineComponent + EnergyStorage + Position, i.e. the three
// components pass 1's view requires. No HeatIntakeComponent unless a test adds
// one, which keeps pass 3 out of the way by default.
entt::entity makeMachine(entt::registry& reg, uint16_t machine_id, int32_t x, int32_t y,
                         int32_t z, int32_t capacity, int32_t current, int32_t max_in = 32,
                         int32_t max_out = 32, EnergyType type = EnergyType::HEAT) {
    auto ent = reg.create();
    reg.emplace<MachineComponent>(ent, machine_id, 0, static_cast<uint32_t>(x),
                                  static_cast<uint32_t>(y), static_cast<uint32_t>(z), 1);
    reg.emplace<EnergyStorage>(ent, capacity, current, max_in, max_out, 0, type);
    reg.emplace<Position>(ent, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                          static_cast<uint32_t>(z));
    return ent;
}

// A controller id. Since gp-wjwr the "is a multiblock controller" fact is
// carried by MachineComponent::mb_id, which SimulationEngine sets to the
// controller id when a multiblock forms (SimulationEngine.cpp:309) and clears
// by removing the whole MachineComponent on teardown (destroyController, :82).
constexpr uint32_t kControllerMbId = 1;

// The exact component set SimulationEngine::onBlockChanged builds for a HEAT
// machine (SimulationEngine.cpp:178-283), with `mb_id` defaulting to
// kControllerMbId so the entity is a live multiblock anchor — the shape a
// formed blast-furnace / large-boiler / LCR controller actually has.
entt::entity makeProductionHeatMachine(entt::registry& reg, uint16_t machine_id, int32_t x,
                                       int32_t y, int32_t z, int32_t heat_stored,
                                       int32_t heat_capacity = 1000,
                                       uint32_t mb_id = kControllerMbId) {
    auto ent = reg.create();
    reg.emplace<Position>(ent, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                          static_cast<uint32_t>(z));
    reg.emplace<Block>(ent, machine_id, 0, 0);
    reg.emplace<MachineComponent>(ent, machine_id, mb_id, static_cast<uint32_t>(x),
                                  static_cast<uint32_t>(y), static_cast<uint32_t>(z), 1);
    reg.emplace<EnergyStorage>(ent, 10000, heat_stored, 32, 32, 0, EnergyType::HEAT);
    HeatIntakeComponent hic;
    hic.heat_stored = heat_stored;
    hic.heat_capacity = heat_capacity;
    reg.emplace<HeatIntakeComponent>(ent, hic);
    return ent;
}

// A pure pass-3 subject: HeatIntakeComponent + Position, plus an optional
// EnergyStorage whose mirroring behaviour is under test. No MachineComponent,
// so pass 1's view does not match and pass 1 cannot interfere.
entt::entity makeHeatStore(entt::registry& reg, int32_t x, int32_t y, int32_t z,
                           int32_t heat_stored, int32_t heat_capacity = 1000) {
    auto ent = reg.create();
    reg.emplace<Position>(ent, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                          static_cast<uint32_t>(z));
    HeatIntakeComponent hic;
    hic.heat_stored = heat_stored;
    hic.heat_capacity = heat_capacity;
    reg.emplace<HeatIntakeComponent>(ent, hic);
    return ent;
}

void addHeatStorage(entt::registry& reg, entt::entity ent, int32_t capacity, int32_t current,
                    EnergyType type = EnergyType::HEAT) {
    reg.emplace<EnergyStorage>(ent, capacity, current, 32, 32, 0, type);
}

void addWater(entt::registry& reg, int32_t x, int32_t y, int32_t z) {
    auto ent = reg.create();
    reg.emplace<Position>(ent, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                          static_cast<uint32_t>(z));
    reg.emplace<Block>(ent, kWaterBlock, 0, 0);
}

int32_t stored(const entt::registry& reg, entt::entity ent) {
    return reg.get<EnergyStorage>(ent).current;
}

int32_t heatStored(const entt::registry& reg, entt::entity ent) {
    return reg.get<HeatIntakeComponent>(ent).heat_stored;
}

constexpr int32_t kBaseCooling =
    static_cast<int32_t>(simcore::HeatConstants::ENVIRONMENT_COOLING_RATE);
constexpr int32_t kWaterCooling = static_cast<int32_t>(simcore::HeatConstants::ENVIRONMENT_COOLING_RATE *
                                                        simcore::HeatConstants::WATER_COOLING_MULTIPLIER);

} // namespace

// ---------------------------------------------------------------------------
// Fixture sanity
// ---------------------------------------------------------------------------

static void test_AdjacencyTransferSystem_registry_declares_heat_topology() {
    auto base = MachineRegistry::LoadFromYaml(GTNH_MACHINES_YAML);
    CHECK(base != nullptr, "machines.yaml loads");
    if (!base) return;

    CHECK(base->Get(kHeatRelay) == nullptr, "kHeatRelay must not collide with a real machine");
    CHECK(base->Get(kHeatGenerator) != nullptr, "heat_generator is in the registry");
    CHECK(base->Get(kHeatFurnace) != nullptr, "heat_furnace is in the registry");

    auto reg = makeRegistry();
    CHECK(reg != nullptr, "makeRegistry succeeds");
    if (reg) checkRegistryPreconditions(*reg);
}

static void test_AdjacencyTransferSystem_empty_registry_is_a_noop() {
    auto reg = makeRegistry();
    CHECK(reg != nullptr, "machines.yaml loads");
    if (!reg) return;

    entt::registry ecs;
    // Empty publisher: the system stores `events_` and never uses it, so no
    // IEventPublisher implementation is needed anywhere in this suite.
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    sys.tick(0.05f);
    sys.tick(1.0f);

    CHECK(ecs.storage<EnergyStorage>().size() == 0, "an empty ECS stays empty");
    CHECK(ecs.storage<OverheatComponent>().size() == 0, "no OverheatComponent is created");
}

// ---------------------------------------------------------------------------
// Pass 1 — adjacent heat transfer
// ---------------------------------------------------------------------------

static void test_AdjacencyTransferSystem_adjacent_transfer_moves_full_producer() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, /*cap*/ 10000, /*cur*/ 5000);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 5000, "the sink receives the producer's whole content");
    CHECK_EQ_I(stored(ecs, gen), 0, "the producer is debited by the same amount");
}

static void test_AdjacencyTransferSystem_transfer_ignores_max_input_and_output() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // maxInput/maxOutput of 1 would rate-limit a sane implementation to 1 HU.
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 4000, /*maxIn*/ 1, /*maxOut*/ 1);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0, /*maxIn*/ 1, /*maxOut*/ 1);

    sys.tick(0.05f);

    // Observed: neither field is consulted anywhere in the transfer path, so
    // 4000 HU move in one tick.
    CHECK_EQ_I(stored(ecs, furnace), 4000, "maxInput does NOT rate-limit adjacency transfer");
    CHECK_EQ_I(stored(ecs, gen), 0, "maxOutput does NOT rate-limit adjacency transfer");
}

static void test_AdjacencyTransferSystem_transfer_into_full_destination_moves_nothing() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 10000); // already full

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 10000, "a full destination stays full");
    CHECK_EQ_I(stored(ecs, gen), 5000, "a full destination pulls nothing from the source");
}

static void test_AdjacencyTransferSystem_transfer_is_clamped_to_remaining_space() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 9000); // only 1000 free

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 10000, "the sink is filled exactly to capacity");
    CHECK_EQ_I(stored(ecs, gen), 4000, "the source keeps the untransferred remainder");
}

static void test_AdjacencyTransferSystem_non_adjacent_source_moves_nothing() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000);
    auto furnace = makeMachine(ecs, kHeatFurnace, /*x=*/2, 0, 0, 10000, 0); // one gap

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 0, "a block two away is not a neighbour");
    CHECK_EQ_I(stored(ecs, gen), 5000, "the source keeps its heat");
}

static void test_AdjacencyTransferSystem_all_six_faces_are_neighbours() {
    auto reg = makeRegistry();

    static const int32_t kOffsets[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                           {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int face = 0; face < 6; ++face) {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        auto gen = makeMachine(ecs, kHeatGenerator, 10, 20, 30, 10000, 700);
        auto furnace = makeMachine(ecs, kHeatFurnace, 10 + kOffsets[face][0],
                                   20 + kOffsets[face][1], 30 + kOffsets[face][2], 10000, 0);
        sys.tick(0.05f);
        CHECK_EQ_I(stored(ecs, furnace), 700, "every one of the six faces is a neighbour");
        CHECK_EQ_I(stored(ecs, gen), 0, "the source is debited on every face");
    }
}

static void test_AdjacencyTransferSystem_source_only_machine_pulls_no_heat() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // heat_generator is a source but NOT a sink, so it must not draw from the
    // adjacent furnace even though the furnace holds a full HEAT charge.
    auto furnace = makeMachine(ecs, kHeatFurnace, 0, 0, 0, 10000, 5000);
    auto gen = makeMachine(ecs, kHeatGenerator, 1, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, gen), 0, "a heat SOURCE is not a sink and pulls nothing");
    CHECK_EQ_I(stored(ecs, furnace), 5000, "the furnace keeps its heat");
}

static void test_AdjacencyTransferSystem_two_sources_never_exceed_sink_capacity() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // Sink at the origin with capacity 100; sources on +x (40) and +z (60).
    auto sink = makeMachine(ecs, kHeatFurnace, 0, 0, 0, 100, 0);
    auto east = makeMachine(ecs, kHeatGenerator, 1, 0, 0, 10000, 40);
    auto south = makeMachine(ecs, kHeatGenerator, 0, 0, 1, 10000, 60);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, sink), 100, "total delivered never exceeds the sink's capacity");
    CHECK_EQ_I(stored(ecs, east) + stored(ecs, south), 0,
               "both sources are debited by exactly what was delivered");
    // The neighbour scan order is +x, -x, +y, -y, +z, -z, so +x is drained
    // first and +z only covers the remaining 60 of need.
    CHECK_EQ_I(stored(ecs, east), 0, "the +x source (first in scan order) is drained first");
    CHECK_EQ_I(stored(ecs, south), 0, "the +z source covers the rest");
}

static void test_AdjacencyTransferSystem_two_sources_fill_short_of_capacity() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // Same layout, but the sink has room for more than the two sources hold.
    auto sink = makeMachine(ecs, kHeatFurnace, 0, 0, 0, 10000, 0);
    auto east = makeMachine(ecs, kHeatGenerator, 1, 0, 0, 10000, 40);
    auto south = makeMachine(ecs, kHeatGenerator, 0, 0, 1, 10000, 60);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, sink), 100, "both adjacent sources are drawn in the same tick");
    CHECK_EQ_I(stored(ecs, east) + stored(ecs, south), 0, "both sources end up empty");
}

static void test_AdjacencyTransferSystem_two_sources_at_one_position_shadow_each_other() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // producersByPos is keyed by packed position, so a second producer at the
    // same coordinate overwrites the first: only ONE source feeds the sink.
    // The producer list is filled by iterating the view in reverse creation
    // order, and each assignment overwrites the previous, so the survivor is
    // the FIRST-created producer (the last one written into the map). World
    // positions are unique in practice, so this is a latent defect — pinned so
    // that fixing it has to be a deliberate change.
    auto sink = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
    auto first = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
    auto second = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 2000);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, first), 0, "the FIRST-created co-located producer is the one that feeds");
    CHECK_EQ_I(stored(ecs, second), 2000, "the second one is shadowed and keeps all its heat");
    CHECK_EQ_I(stored(ecs, sink), 1000, "the sink receives only the surviving producer's charge");
}

static void test_AdjacencyTransferSystem_transfer_mirrors_into_heat_intake() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
    // The block-entity creation path gives every HEAT machine a
    // HeatIntakeComponent (SimulationEngine.cpp:263).
    HeatIntakeComponent hic;
    hic.heat_stored = 0;
    hic.heat_capacity = 1000;
    ecs.emplace<HeatIntakeComponent>(furnace, hic);

    sys.tick(0.05f);

    // Pass 1 mirrors the 5000 HU into heat_stored, then pass 3 cools 4 HU and
    // writes the result back into current, so both fields land on 4996.
    CHECK_EQ_I(heatStored(ecs, furnace), 5000 - kBaseCooling,
               "heat_stored mirrors the transferred heat, minus pass-3 cooling");
    CHECK_EQ_I(stored(ecs, furnace), 5000 - kBaseCooling,
               "EnergyStorage::current holds the same value");
    CHECK_EQ_I(stored(ecs, gen), 0, "the producer was debited by the transfer");
}

static void test_AdjacencyTransferSystem_transfer_works_without_heat_intake() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // HeatIntakeComponent is optional (try_get) — transfer must still happen,
    // and pass 3 has nothing to cool.
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 5000, "transfer does not require HeatIntakeComponent");
    CHECK_EQ_I(stored(ecs, gen), 0, "the producer was debited by the transfer");
    CHECK(!ecs.all_of<HeatIntakeComponent>(furnace),
          "the system does not create HeatIntakeComponent itself");
}

static void test_AdjacencyTransferSystem_zero_heat_source_moves_nothing() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 0);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 0, "an empty source is not a producer");
    CHECK_EQ_I(stored(ecs, gen), 0, "the empty source is untouched");
}

static void test_AdjacencyTransferSystem_non_heat_energy_is_never_transferred() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // Same machines, but STEAM storage: the energy-type gate rejects them.
    auto steamGen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000, 32, 32,
                                EnergyType::STEAM);
    auto steamSink = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0, 32, 32,
                                 EnergyType::STEAM);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, steamSink), 0, "STEAM is not moved by the HEAT network");
    CHECK_EQ_I(stored(ecs, steamGen), 5000, "the STEAM source is untouched");
}

// ---------------------------------------------------------------------------
// Pass 1 — chains (gp-c41q)
// ---------------------------------------------------------------------------

static void test_AdjacencyTransferSystem_chain_propagates_the_whole_distance_in_one_tick() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // heat_generator (source) — relay (source+sink) — heat_furnace (sink).
    // Before gp-c41q the chain advanced ONE BLOCK PER TICK, so the furnace saw
    // nothing in tick 1 and the charge arrived in tick 2. Heat now traverses
    // the whole chain in the tick it was produced.
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
    auto relay = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 0);
    auto furnace = makeMachine(ecs, kHeatFurnace, 2, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, gen), 0, "the generator is drained in tick 1");
    CHECK_EQ_I(stored(ecs, relay), 0,
               "gp-c41q: the relay passes the charge on in the SAME tick it takes it");
    CHECK_EQ_I(stored(ecs, furnace), 1000,
               "gp-c41q: and the sink at the far end receives it in that same tick");
}

// The defect was order-dependent, so the test that matters is the one that runs
// the SAME layout in BOTH creation orders. Reverse view order used to decide
// how far heat travelled; it must no longer do so.
static void test_AdjacencyTransferSystem_chain_order_does_not_change_how_far_heat_travels() {
    auto reg = makeRegistry();

    // Order A: created downstream-first — the case that already worked, because
    // the upstream source happened to be visited first.
    {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        auto furnace = makeMachine(ecs, kHeatFurnace, 2, 0, 0, 10000, 0);
        auto relay = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 0);
        auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
        sys.tick(0.05f);
        CHECK_EQ_I(stored(ecs, furnace), 1000, "order A: the sink gets the whole charge");
        CHECK_EQ_I(stored(ecs, relay), 0, "order A: the relay is left empty");
        CHECK_EQ_I(stored(ecs, gen), 0, "order A: the generator is drained");
    }

    // Order B: created upstream-first — the case that used to need two ticks,
    // because the view yielded (furnace, relay, gen) and the sink was visited
    // before anything could reach it.
    {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
        auto relay = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 0);
        auto furnace = makeMachine(ecs, kHeatFurnace, 2, 0, 0, 10000, 0);
        sys.tick(0.05f);
        CHECK_EQ_I(stored(ecs, furnace), 1000,
                   "gp-c41q: order B reaches the sink in one tick too — creation "
                   "order no longer decides propagation");
        CHECK_EQ_I(stored(ecs, relay), 0, "order B: the relay is left empty");
        CHECK_EQ_I(stored(ecs, gen), 0, "order B: the generator is drained");
    }
}

// A relay that already holds heat must not double-count: the sweep refills it
// from upstream and then forwards everything it holds, once.
static void test_AdjacencyTransferSystem_a_warm_relay_forwards_everything_in_one_tick() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // The relay starts with 500 of its own. Before gp-c41q tick 1 could only
    // deliver the pre-existing 500 to the sink and the 1000 from the generator
    // had to wait for tick 2; the sink ended tick 1 holding 1500 out of 1500
    // across two ticks. Now the sink has all 1500 after one tick.
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
    auto relay = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 500);
    auto furnace = makeMachine(ecs, kHeatFurnace, 2, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, gen), 0, "the generator is fully drained in tick 1");
    CHECK_EQ_I(stored(ecs, relay), 0, "the relay ends empty");
    CHECK_EQ_I(stored(ecs, furnace), 1500,
               "gp-c41q: the sink holds all 1500 after ONE tick, not two");
}

static void test_AdjacencyTransferSystem_four_block_chain_reaches_the_sink_in_one_tick() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 500);
    auto r1 = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 0);
    auto r2 = makeMachine(ecs, kHeatRelay, 2, 0, 0, 10000, 0);
    auto furnace = makeMachine(ecs, kHeatFurnace, 3, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 500,
               "gp-c41q: three hops complete in one tick — the old sweep needed three");
    CHECK_EQ_I(stored(ecs, gen) + stored(ecs, r1) + stored(ecs, r2), 0,
               "and nothing is stranded upstream");
}

// The sweep is bounded by REACHABILITY, not by distance: a sink with no heat
// source anywhere adjacent is never visited, exactly as before the fix. Pinned
// so that "visit everything in BFS order" cannot be read as "visit everything".
static void test_AdjacencyTransferSystem_an_unreachable_sink_is_still_skipped() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // A source two blocks away from the sink, with a gap: the BFS never reaches
    // the sink, so it is not in the sweep at all.
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
    auto furnace = makeMachine(ecs, kHeatFurnace, 2, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, furnace), 0, "a sink no source can reach is never visited");
    CHECK_EQ_I(stored(ecs, gen), 1000, "and the source keeps all its heat");
}
// A PURE SINK cannot relay: pass 1 only ever moves heat out of a heat SOURCE,
// so the BFS must not expand through a sink-only machine. Two furnaces in a row
// are a sink, a gap, and then the real sink — the charge stops at the first.
static void test_AdjacencyTransferSystem_a_pure_sink_does_not_relay() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // gen(source) — furnace(sink) — furnace(sink). The middle furnace is a
    // heat SINK and not a source (heat_furnace has energy_in HEAT, no
    // energy_out), so it cannot pass its charge on.
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
    auto first = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
    auto second = makeMachine(ecs, kHeatFurnace, 2, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, first), 1000, "the first sink takes the charge");
    CHECK_EQ_I(stored(ecs, second), 0,
               "gp-c41q: a pure sink does not relay — heat stops at the first one");
    CHECK_EQ_I(stored(ecs, gen), 0, "and the source is drained");
}

// Determinism: two blocks at the SAME distance from a source are both visited
// in the same tick, and the order between them must not depend on which one
// entt happens to yield first. Both are filled; the split is decided by the
// documented +x, -x, +y, -y, +z, -z neighbour scan order, not by creation order.
static void test_AdjacencyTransferSystem_same_distance_sinks_both_fill_in_one_tick() {
    auto reg = makeRegistry();

    for (int createSinksFirst = 0; createSinksFirst < 2; ++createSinksFirst) {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        entt::entity east, south, gen;
        if (createSinksFirst) {
            east = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 1000, 0);
            south = makeMachine(ecs, kHeatFurnace, 0, 0, 1, 1000, 0);
            gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
        } else {
            gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
            east = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 1000, 0);
            south = makeMachine(ecs, kHeatFurnace, 0, 0, 1, 1000, 0);
        }
        sys.tick(0.05f);
        CHECK_EQ_I(stored(ecs, east) + stored(ecs, south), 1000,
                   "both sinks at distance 1 are filled in the same tick");
        CHECK_EQ_I(stored(ecs, gen), 0,
                   "and the single source is drained exactly once, never twice");
    }
}

// The full four-block chain, ticked repeatedly, must be STABLE — the sweep must
// not oscillate or double-transfer. Before gp-c41q the chain needed three ticks
// to settle; now it is settled after the first and must stay settled.
static void test_AdjacencyTransferSystem_a_settled_chain_does_not_oscillate() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 500);
    auto r1 = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 0);
    auto r2 = makeMachine(ecs, kHeatRelay, 2, 0, 0, 10000, 0);
    auto furnace = makeMachine(ecs, kHeatFurnace, 3, 0, 0, 10000, 0);

    for (int tick = 0; tick < 5; ++tick) {
        sys.tick(0.05f);
        const int32_t total =
            stored(ecs, gen) + stored(ecs, r1) + stored(ecs, r2) + stored(ecs, furnace);
        CHECK_EQ_I(total, 500, "the chain conserves heat on every tick");
        CHECK_EQ_I(stored(ecs, furnace), 500,
                   "and the sink keeps the charge instead of the chain drifting");
        CHECK_EQ_I(stored(ecs, r1) + stored(ecs, r2), 0, "no heat is stranded mid-chain");
    }
}

// Energy is conserved no matter how long the chain is: the sweep moves heat,
// it does not create it.
static void test_AdjacencyTransferSystem_long_chain_conserves_energy() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    std::vector<entt::entity> chain;
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 777);
    chain.push_back(gen);
    for (int i = 1; i < 8; ++i) {
        chain.push_back(makeMachine(ecs, kHeatRelay, i, 0, 0, 10000, 0));
    }
    auto sink = makeMachine(ecs, kHeatFurnace, 8, 0, 0, 10000, 0);
    chain.push_back(sink);

    sys.tick(0.05f);

    int32_t total = 0;
    for (auto e : chain) total += stored(ecs, e);
    CHECK_EQ_I(total, 777, "an eight-block chain conserves the charge exactly");
    CHECK_EQ_I(stored(ecs, sink), 777, "and the sink at the end holds all of it");
}

// EnTT yields this view in REVERSE creation order (the last entity created is
// The view still yields reverse creation order — that is an EnTT property, and
// the sweep now depends on it only as the TIE-BREAK between two blocks at the
// same distance from a source, never to decide how far heat travels. Pinned so
// the tie-break is documented rather than discovered.
static void test_AdjacencyTransferSystem_view_yields_reverse_creation_order() {
    auto reg = makeRegistry();
    entt::registry ecs;

    auto first = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 100);
    auto second = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
    auto third = makeMachine(ecs, kHeatRelay, 2, 0, 0, 10000, 100);

    std::vector<entt::entity> seen;
    for (auto e : ecs.view<MachineComponent, EnergyStorage, Position>()) {
        seen.push_back(e);
    }

    CHECK_EQ_I(static_cast<int64_t>(seen.size()), 3, "all three machines are in the view");
    if (seen.size() == 3) {
        CHECK(seen[0] == third, "the LAST-created entity is visited first");
        CHECK(seen[1] == second, "then the middle one");
        CHECK(seen[2] == first, "the FIRST-created entity is visited last");
    }
}

// The pair that used to disagree with itself. A warm source next to a sink
// completed the hop in one tick when the source was created second, and needed
// two when the sink was created second. gp-c41q removed the difference; this
// runs both and requires them to agree.
static void test_AdjacencyTransferSystem_a_single_hop_completes_in_one_tick_either_way() {
    auto reg = makeRegistry();

    {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
        auto relay = makeMachine(ecs, kHeatRelay, 0, 0, 0, 10000, 300);
        sys.tick(0.05f);
        CHECK_EQ_I(stored(ecs, relay), 0, "sink-first: the source is drained");
        CHECK_EQ_I(stored(ecs, furnace), 300, "sink-first: the sink receives it");
    }

    {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        auto relay = makeMachine(ecs, kHeatRelay, 0, 0, 0, 10000, 300);
        auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
        sys.tick(0.05f);
        CHECK_EQ_I(stored(ecs, relay), 0, "source-first: the source is drained");
        CHECK_EQ_I(stored(ecs, furnace), 300,
                   "gp-c41q: source-first now gives the SAME result — order no "
                   "longer decides whether the hop completes");
    }
}

// The cold-source three-block case that the old ordering stranded for a full
// tick. Kept as its own test because it is the shape that actually occurs in
// production: a generator feeding a fresh coil run.
static void test_AdjacencyTransferSystem_a_cold_chain_reaches_the_sink_in_one_tick() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // gen — relay — furnace, all cold except the generator, created upstream
    // first (the order that used to visit the sink before anything reached it).
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 1000);
    auto relay = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 0);
    auto furnace = makeMachine(ecs, kHeatFurnace, 2, 0, 0, 10000, 0);

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, gen), 0, "the generator is drained in tick 1");
    CHECK_EQ_I(stored(ecs, relay), 0, "the relay forwards the same tick it is filled");
    CHECK_EQ_I(stored(ecs, furnace), 1000,
               "gp-c41q: the sink is no longer left waiting for a second tick");
}

static void test_AdjacencyTransferSystem_four_block_chain_conserves_energy() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 500);
    auto r1 = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 0);
    auto r2 = makeMachine(ecs, kHeatRelay, 2, 0, 0, 10000, 0);
    auto furnace = makeMachine(ecs, kHeatFurnace, 3, 0, 0, 10000, 0);

    for (int tick = 0; tick < 3; ++tick) {
        sys.tick(0.05f);
        int32_t total = stored(ecs, gen) + stored(ecs, r1) + stored(ecs, r2) + stored(ecs, furnace);
        CHECK_EQ_I(total, 500, "pass 1 conserves heat across a four-block chain");
    }
    CHECK_EQ_I(stored(ecs, furnace), 500, "the sink receives the whole charge");
    CHECK_EQ_I(stored(ecs, gen) + stored(ecs, r1) + stored(ecs, r2), 0, "upstream is empty");
}

static void test_AdjacencyTransferSystem_adjacent_source_and_sink_pair_drains_each_other() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // Two relays side by side, both source and sink, both warm. Each is a
    // producer and each is a sink, so the pass is: visit the LAST-created
    // relay first, let it drain the other, then visit the first-created relay
    // and let it drain that back. Observed: the charge ping-pongs — the second
    // relay ends up with everything on tick 1, and the two swap the whole
    // 1500 on every subsequent tick. Nothing is lost, but the pair never
    // settles and the outcome depends on view iteration order.
    auto a = makeMachine(ecs, kHeatRelay, 0, 0, 0, 10000, 1000);
    auto b = makeMachine(ecs, kHeatRelay, 1, 0, 0, 10000, 500);

    sys.tick(0.05f);
    CHECK_EQ_I(stored(ecs, a) + stored(ecs, b), 1500, "the pair conserves heat on tick 1");
    CHECK_EQ_I(stored(ecs, a), 1500, "b is visited first, so a absorbs the whole charge");
    CHECK_EQ_I(stored(ecs, b), 0, "b is left empty on tick 1");

    sys.tick(0.05f);
    CHECK_EQ_I(stored(ecs, a) + stored(ecs, b), 1500, "the pair conserves heat on tick 2");
    CHECK_EQ_I(stored(ecs, a), 0, "the pair swaps the whole charge back on the next tick");
    CHECK_EQ_I(stored(ecs, b), 1500, "b is back in possession on tick 2");
}

static void test_AdjacencyTransferSystem_dt_is_ignored() {
    auto reg = makeRegistry();
    for (float dt : {0.0f, 0.05f, 1.0f, 100.0f}) {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000);
        auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);

        sys.tick(dt);

        CHECK_EQ_I(stored(ecs, furnace), 5000, "the transfer amount is independent of dt");
        CHECK_EQ_I(stored(ecs, gen), 0, "the source debit is independent of dt");
    }
}

static void test_AdjacencyTransferSystem_repeated_ticks_do_not_double_transfer() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 3000);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);

    sys.tick(0.05f);
    CHECK_EQ_I(stored(ecs, furnace), 3000, "the first tick delivers everything");
    for (int i = 0; i < 5; ++i) sys.tick(0.05f);
    CHECK_EQ_I(stored(ecs, furnace), 3000, "an empty source cannot keep feeding the sink");
    CHECK_EQ_I(stored(ecs, gen), 0, "the source stays empty");
}

// ---------------------------------------------------------------------------
// Pass 2 — overheat detection, and its multiblock gate
// ---------------------------------------------------------------------------

static void test_AdjacencyTransferSystem_overheat_pass_requires_a_controller_mb_id() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // A production-shaped HEAT machine at twice the critical ratio, but a
    // SINGLE-BLOCK machine (mb_id == 0). The overheat logic is fully primed —
    // only the multiblock gate keeps it out. 30 ticks of pass-3 cooling (4 HU
    // each) still leaves the ratio above 1.0, so the assertion below cannot
    // pass by accident.
    const int32_t stored0 = 2000;
    auto furnace = makeProductionHeatMachine(ecs, kHeatFurnace, 5, 64, 5, stored0,
                                             1000, /*mb_id=*/0);

    CHECK_EQ_I(static_cast<int64_t>(ecs.get<MachineComponent>(furnace).mb_id), 0,
               "precondition: a single-block machine has no controller id");
    for (int tick = 0; tick < 30; ++tick) sys.tick(0.05f);

    CHECK(!ecs.all_of<OverheatComponent>(furnace),
          "a single-block machine is outside the gate and never gets OverheatComponent");
    CHECK(ecs.all_of<HeatIntakeComponent>(furnace), "the entity itself survives");
    CHECK(ecs.get<HeatIntakeComponent>(furnace).ratio() >=
              simcore::HeatConstants::OVERHEAT_CRITICAL_THRESHOLD,
          "the ratio is still at CRITICAL after 30 ticks, so only the gate is missing");
}

static void test_AdjacencyTransferSystem_overheat_pass_fires_for_a_controller() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // Identical to the previous test, but with a nonzero mb_id — the shape a
    // formed multiblock controller anchor actually has in production. This is
    // the same test that used to be IMPOSSIBLE to pass, because pass 2 keyed on
    // a MultiblockController ECS component that no production path ever emplaced.
    auto furnace = makeProductionHeatMachine(ecs, kHeatFurnace, 5, 64, 5, 1000);

    sys.tick(0.05f);

    CHECK(ecs.all_of<OverheatComponent>(furnace),
          "a multiblock controller anchor (mb_id != 0) DOES enter the pass-2 gate");
    if (ecs.all_of<OverheatComponent>(furnace)) {
        const auto& oh = ecs.get<OverheatComponent>(furnace);
        CHECK(oh.state == OverheatState::CRITICAL, "a ratio of 1.0 maps to CRITICAL");
        CHECK_EQ_I(oh.ticks_at_critical, 0, "a freshly emplaced component starts its counter at 0");
    }
}

static void test_AdjacencyTransferSystem_overheat_pass_skips_a_machine_with_no_machine_component() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // HeatIntakeComponent alone (a bare heat store, no MachineComponent): the
    // gate reads mb_id off MachineComponent, so this must be skipped rather
    // than crash.
    auto store = makeHeatStore(ecs, 5, 64, 5, 1000);

    sys.tick(0.05f);

    CHECK(!ecs.all_of<OverheatComponent>(store),
          "an entity with no MachineComponent never enters the pass-2 gate");
}

static void test_AdjacencyTransferSystem_overheat_critical_and_warning_thresholds() {
    auto reg = makeRegistry();

    struct Case {
        int32_t stored;
        bool expect_component;
        OverheatState expected;
    };
    static const Case kCases[] = {
        {1000, true, OverheatState::CRITICAL},  // ratio 1.000 — at the critical mark
        {1001, true, OverheatState::CRITICAL},  // ratio 1.001
        {900, true, OverheatState::WARNING},    // ratio 0.900 — at the warning mark
        {999, true, OverheatState::WARNING},    // ratio 0.999
        {899, false, OverheatState::NONE},      // ratio 0.899 — below the warning mark
        {0, false, OverheatState::NONE},
    };

    for (const auto& c : kCases) {
        entt::registry ecs;
        simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());
        auto ent = makeProductionHeatMachine(ecs, kHeatFurnace, 5, 64, 5, c.stored);
        sys.tick(0.05f);

        CHECK_EQ_I(ecs.all_of<simcore::OverheatComponent>(ent), c.expect_component,
                   "an OverheatComponent exists exactly above the warning threshold");
        if (ecs.all_of<simcore::OverheatComponent>(ent)) {
            CHECK(ecs.get<simcore::OverheatComponent>(ent).state == c.expected,
                  "heat_stored maps to the documented overheat state");
        }
    }
}

static void test_AdjacencyTransferSystem_overheat_absent_below_warning_threshold() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeProductionHeatMachine(ecs, kHeatFurnace, 5, 64, 5, 100);

    sys.tick(0.05f);

    CHECK(!ecs.all_of<OverheatComponent>(ent), "a cool machine gets no OverheatComponent");
}

static void test_AdjacencyTransferSystem_overheat_is_removed_when_cooling() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeProductionHeatMachine(ecs, kHeatFurnace, 5, 64, 5, 1000); // CRITICAL
    sys.tick(0.05f);
    CHECK(ecs.all_of<OverheatComponent>(ent), "precondition: the machine is overheated");

    ecs.get<HeatIntakeComponent>(ent).heat_stored = 0;
    sys.tick(0.05f);

    CHECK(!ecs.all_of<OverheatComponent>(ent), "cooling below the threshold clears the flag");
}

static void test_AdjacencyTransferSystem_overheat_update_preserves_critical_counter() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeProductionHeatMachine(ecs, kHeatFurnace, 5, 64, 5, 1000);
    auto& oh = ecs.emplace<OverheatComponent>(ent);
    oh.state = OverheatState::CRITICAL;
    oh.ticks_at_critical = 7;

    sys.tick(0.05f);

    // The CRITICAL branch updates .state in place (try_get + assign) so the
    // fuse accumulated by ExplosionSystem survives; only the emplace path
    // would reset the counter to 0.
    CHECK_EQ_I(ecs.get<OverheatComponent>(ent).ticks_at_critical, 7,
               "an existing OverheatComponent keeps its ticks_at_critical");
    CHECK(ecs.get<OverheatComponent>(ent).state == OverheatState::CRITICAL,
          "the state is refreshed to CRITICAL");
}

static void test_AdjacencyTransferSystem_overheat_re_arms_from_warning_to_critical() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeProductionHeatMachine(ecs, kHeatFurnace, 5, 64, 5, 950); // WARNING
    sys.tick(0.05f);
    CHECK(ecs.get<OverheatComponent>(ent).state == OverheatState::WARNING, "precondition: WARNING");

    ecs.get<HeatIntakeComponent>(ent).heat_stored = 1000;
    sys.tick(0.05f);

    CHECK(ecs.get<OverheatComponent>(ent).state == OverheatState::CRITICAL,
          "WARNING escalates to CRITICAL in place");
}

static void test_AdjacencyTransferSystem_overheat_gate_is_live_for_a_populated_world() {
    // The counterpart of the old "the pass-2 view is empty for real ECS" test,
    // which asserted the gp-wjwr dead view as a runtime observation. That test
    // is now inverted: the same production-shaped, fully populated world DOES
    // enter the pass-2 gate, and the overheated machines get OverheatComponent.
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    for (int i = 0; i < 8; ++i) {
        makeProductionHeatMachine(ecs, kHeatFurnace, i, 64, 5, 2000);
        makeProductionHeatMachine(ecs, kHeatGenerator, i, 65, 5, 2000);
    }
    addWater(ecs, 0, 64, 6);

    sys.tick(0.05f);

    size_t gated = 0;
    for (auto ent : ecs.view<HeatIntakeComponent, MachineComponent>()) {
        if (ecs.get<MachineComponent>(ent).mb_id != 0) ++gated;
    }
    CHECK_EQ_I(static_cast<int64_t>(gated), 16,
               "every controller-shaped HEAT machine is inside the pass-2 gate");
    CHECK_EQ_I(static_cast<int64_t>(ecs.storage<OverheatComponent>().size()), 16,
               "and each one that is above the warning ratio gets an OverheatComponent");
}

// ---------------------------------------------------------------------------
// Pass 3 — environment cooling
// ---------------------------------------------------------------------------

static void test_AdjacencyTransferSystem_cooling_reduces_heat_stored() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeHeatStore(ecs, 5, 64, 5, 100);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 100 - kBaseCooling,
               "a dry machine sheds ENVIRONMENT_COOLING_RATE per tick");
}

static void test_AdjacencyTransferSystem_cooling_clamps_to_stored_heat() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // The amount is a truncating (int32_t) cast of a float rate, then clamped
    // to the heat actually stored. A machine holding less than one tick's
    // worth is emptied, never driven negative.
    auto ent = makeHeatStore(ecs, 5, 64, 5, 1);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 0, "cooling clamps at zero rather than underflowing");
    CHECK(heatStored(ecs, ent) >= 0, "heat_stored is never negative");
}

static void test_AdjacencyTransferSystem_water_adjacent_cooling_is_tripled() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    addWater(ecs, 6, 64, 5); // +x neighbour of `wet`
    auto wet = makeHeatStore(ecs, 5, 64, 5, 100);
    auto dry = makeHeatStore(ecs, 5, 63, 5, 100); // no water neighbour

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, wet), 100 - kWaterCooling, "water triples the cooling rate");
    CHECK_EQ_I(heatStored(ecs, dry), 100 - kBaseCooling,
               "a machine away from water cools at the base rate");
}

static void test_AdjacencyTransferSystem_distant_water_does_not_cool_faster() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    addWater(ecs, 7, 64, 5); // two blocks away
    auto ent = makeHeatStore(ecs, 5, 64, 5, 100);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 100 - kBaseCooling, "only face-adjacent water counts");
}

static void test_AdjacencyTransferSystem_zero_heat_is_not_cooled() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeHeatStore(ecs, 5, 64, 5, 0);
    addHeatStorage(ecs, ent, 10000, 555);
    addWater(ecs, 6, 64, 5);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 0, "an empty heat store is skipped entirely");
    CHECK_EQ_I(stored(ecs, ent), 555,
               "and its EnergyStorage::current is NOT mirrored either (the early return)");
}

static void test_AdjacencyTransferSystem_cooling_overwrites_energy_storage_current() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // heat_stored and EnergyStorage::current have drifted apart. Pass 3 ends
    // with `energy->current = hic.heat_stored`, an unconditional overwrite with
    // no capacity clamp, so 7777 HU are silently destroyed. This is the mirror
    // image of the pass-1 sync, and the two disagree about which field is
    // authoritative.
    auto ent = makeHeatStore(ecs, 5, 64, 5, 100);
    addHeatStorage(ecs, ent, 10000, 7777);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 100 - kBaseCooling, "heat_stored is cooled by the base rate");
    CHECK_EQ_I(stored(ecs, ent), 100 - kBaseCooling,
               "current is force-overwritten from the cooled heat_stored, losing 7777 HU");
}

static void test_AdjacencyTransferSystem_cooling_skips_non_heat_energy_storage() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeHeatStore(ecs, 5, 64, 5, 100);
    addHeatStorage(ecs, ent, 10000, 555, EnergyType::STEAM);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 100 - kBaseCooling, "heat_stored is still cooled");
    CHECK_EQ_I(stored(ecs, ent), 555, "a non-HEAT EnergyStorage is left alone");
}

static void test_AdjacencyTransferSystem_cooling_needs_position() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // The pass-3 view is <HeatIntakeComponent, Position>: no Position means
    // no cooling at all.
    auto ent = ecs.create();
    HeatIntakeComponent hic;
    hic.heat_stored = 100;
    ecs.emplace<HeatIntakeComponent>(ent, hic);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 100, "without Position the cooling view is empty");
}

static void test_AdjacencyTransferSystem_water_cooling_never_goes_negative() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    auto ent = makeHeatStore(ecs, 5, 64, 5, 2);
    addWater(ecs, 6, 64, 5);

    sys.tick(0.05f);

    CHECK_EQ_I(heatStored(ecs, ent), 0, "water cooling clamps at zero rather than underflowing");
}

// ---------------------------------------------------------------------------
// Pass interaction
// ---------------------------------------------------------------------------

static void test_AdjacencyTransferSystem_transfer_then_cooling_same_tick() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // Pass 1 mirrors transferred heat into heat_stored; pass 3 then cools it
    // and writes it back, so a machine adjacent to water nets 12 HU less than
    // a dry one after a single tick.
    auto dryGen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 5000);
    auto dryFurnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
    HeatIntakeComponent dh;
    dh.heat_stored = 0;
    dh.heat_capacity = 1000;
    ecs.emplace<HeatIntakeComponent>(dryFurnace, dh);

    auto wetGen = makeMachine(ecs, kHeatGenerator, 0, 65, 0, 10000, 5000);
    auto wetFurnace = makeMachine(ecs, kHeatFurnace, 1, 65, 0, 10000, 0);
    HeatIntakeComponent wh;
    wh.heat_stored = 0;
    wh.heat_capacity = 1000;
    ecs.emplace<HeatIntakeComponent>(wetFurnace, wh);
    addWater(ecs, 2, 65, 0); // +x neighbour of the wet furnace

    sys.tick(0.05f);

    CHECK_EQ_I(stored(ecs, dryFurnace), 5000 - kBaseCooling, "dry: 5000 in, 4 cooled");
    CHECK_EQ_I(stored(ecs, wetFurnace), 5000 - kWaterCooling, "wet: 5000 in, 12 cooled");
    CHECK_EQ_I(stored(ecs, dryGen) + stored(ecs, wetGen), 0, "both sources are drained");
    CHECK_EQ_I(heatStored(ecs, wetFurnace), 5000 - kWaterCooling, "heat_stored tracks current");
}

static void test_AdjacencyTransferSystem_cooling_starves_the_furnace() {
    auto reg = makeRegistry();
    entt::registry ecs;
    simcore::AdjacencyTransferSystem sys(ecs, *reg, std::shared_ptr<simcore::IEventPublisher>());

    // A generator feeding exactly one tick of cooling worth of heat: the
    // furnace receives 4 HU and cooling spends all of it, so the machine never
    // makes progress and never overheats. Pinned because it is the equilibrium
    // that a flat per-tick cooling amount (not a fraction) implies.
    auto gen = makeMachine(ecs, kHeatGenerator, 0, 0, 0, 10000, 0);
    auto furnace = makeMachine(ecs, kHeatFurnace, 1, 0, 0, 10000, 0);
    HeatIntakeComponent hic;
    hic.heat_stored = 0;
    hic.heat_capacity = 1000;
    ecs.emplace<HeatIntakeComponent>(furnace, hic);

    for (int tick = 0; tick < 10; ++tick) {
        ecs.get<EnergyStorage>(gen).current = kBaseCooling; // keep the generator topped up
        sys.tick(0.05f);
    }

    CHECK_EQ_I(stored(ecs, furnace), 0, "heat in equals heat out: the furnace is pinned at zero");
    CHECK_EQ_I(static_cast<int64_t>(ecs.get<MachineComponent>(furnace).mb_id), 0,
               "precondition: a makeMachine() subject is a single-block machine, so it is "
               "outside the overheat gate entirely");
    CHECK(!ecs.all_of<OverheatComponent>(furnace), "a zero-heat furnace never overheats");
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== adjacency_transfer_system test suite ===\n\n");

    TEST(AdjacencyTransferSystem_registry_declares_heat_topology);
    TEST(AdjacencyTransferSystem_empty_registry_is_a_noop);
    TEST(AdjacencyTransferSystem_adjacent_transfer_moves_full_producer);
    TEST(AdjacencyTransferSystem_transfer_ignores_max_input_and_output);
    TEST(AdjacencyTransferSystem_transfer_into_full_destination_moves_nothing);
    TEST(AdjacencyTransferSystem_transfer_is_clamped_to_remaining_space);
    TEST(AdjacencyTransferSystem_non_adjacent_source_moves_nothing);
    TEST(AdjacencyTransferSystem_all_six_faces_are_neighbours);
    TEST(AdjacencyTransferSystem_source_only_machine_pulls_no_heat);
    TEST(AdjacencyTransferSystem_two_sources_never_exceed_sink_capacity);
    TEST(AdjacencyTransferSystem_two_sources_fill_short_of_capacity);
    TEST(AdjacencyTransferSystem_two_sources_at_one_position_shadow_each_other);
    TEST(AdjacencyTransferSystem_transfer_mirrors_into_heat_intake);
    TEST(AdjacencyTransferSystem_transfer_works_without_heat_intake);
    TEST(AdjacencyTransferSystem_zero_heat_source_moves_nothing);
    TEST(AdjacencyTransferSystem_non_heat_energy_is_never_transferred);
    TEST(AdjacencyTransferSystem_chain_propagates_the_whole_distance_in_one_tick);
    TEST(AdjacencyTransferSystem_chain_order_does_not_change_how_far_heat_travels);
    TEST(AdjacencyTransferSystem_a_warm_relay_forwards_everything_in_one_tick);
    TEST(AdjacencyTransferSystem_four_block_chain_reaches_the_sink_in_one_tick);
    TEST(AdjacencyTransferSystem_an_unreachable_sink_is_still_skipped);
    TEST(AdjacencyTransferSystem_a_pure_sink_does_not_relay);
    TEST(AdjacencyTransferSystem_same_distance_sinks_both_fill_in_one_tick);
    TEST(AdjacencyTransferSystem_a_settled_chain_does_not_oscillate);
    TEST(AdjacencyTransferSystem_long_chain_conserves_energy);
    TEST(AdjacencyTransferSystem_view_yields_reverse_creation_order);
    TEST(AdjacencyTransferSystem_a_single_hop_completes_in_one_tick_either_way);
    TEST(AdjacencyTransferSystem_a_cold_chain_reaches_the_sink_in_one_tick);
    TEST(AdjacencyTransferSystem_four_block_chain_conserves_energy);
    TEST(AdjacencyTransferSystem_adjacent_source_and_sink_pair_drains_each_other);
    TEST(AdjacencyTransferSystem_dt_is_ignored);
    TEST(AdjacencyTransferSystem_repeated_ticks_do_not_double_transfer);
    TEST(AdjacencyTransferSystem_overheat_pass_requires_a_controller_mb_id);
    TEST(AdjacencyTransferSystem_overheat_pass_fires_for_a_controller);
    TEST(AdjacencyTransferSystem_overheat_pass_skips_a_machine_with_no_machine_component);
    TEST(AdjacencyTransferSystem_overheat_critical_and_warning_thresholds);
    TEST(AdjacencyTransferSystem_overheat_absent_below_warning_threshold);
    TEST(AdjacencyTransferSystem_overheat_is_removed_when_cooling);
    TEST(AdjacencyTransferSystem_overheat_update_preserves_critical_counter);
    TEST(AdjacencyTransferSystem_overheat_re_arms_from_warning_to_critical);
    TEST(AdjacencyTransferSystem_overheat_gate_is_live_for_a_populated_world);
    TEST(AdjacencyTransferSystem_cooling_reduces_heat_stored);
    TEST(AdjacencyTransferSystem_cooling_clamps_to_stored_heat);
    TEST(AdjacencyTransferSystem_water_adjacent_cooling_is_tripled);
    TEST(AdjacencyTransferSystem_distant_water_does_not_cool_faster);
    TEST(AdjacencyTransferSystem_zero_heat_is_not_cooled);
    TEST(AdjacencyTransferSystem_cooling_overwrites_energy_storage_current);
    TEST(AdjacencyTransferSystem_cooling_skips_non_heat_energy_storage);
    TEST(AdjacencyTransferSystem_cooling_needs_position);
    TEST(AdjacencyTransferSystem_water_cooling_never_goes_negative);
    TEST(AdjacencyTransferSystem_transfer_then_cooling_same_tick);
    TEST(AdjacencyTransferSystem_cooling_starves_the_furnace);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}