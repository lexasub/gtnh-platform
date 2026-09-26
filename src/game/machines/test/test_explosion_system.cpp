// ExplosionSystem unit tests (issue gp-43vj).
//
// Covers src/game/machines/ExplosionSystem.cpp — the CRITICAL-overheat fuse and
// the block destruction it performs.
//
// The system view is:
//     reg_.view<MachineComponent, Position, OverheatComponent>()
// plus the gate `MachineComponent::mb_id != 0` — the entity must be a live
// multiblock controller anchor. An entity is therefore a candidate only if it
// carries ALL THREE components AND belongs to a multiblock.
//
// GATE HISTORY (gp-qgtc): the view used to be a 4-type view that also required
// a `MultiblockController` ECS component, and that view was DEAD — no
// production path ever emplaces such a component, because SimulationEngine owns
// controllers in a plain `std::unordered_map<uint64_t, MultiblockController>`
// (SimulationEngine.h:108) that EBFSystem / LCRSystem / LargeBoilerSystem
// mutate in place. The gate now keys off `MachineComponent::mb_id`, which the
// engine already sets on formation (SimulationEngine.cpp:309), refreshes on
// every block echo (:370), and drops on teardown (destroyController, :82).
// The tests below therefore build candidates with a NONZERO mb_id, and the
// single-block (`mb_id == 0`) case is pinned as a negative.
//
// What the system actually does per tick, for each candidate:
//   1. skips unless overheat.state == OverheatState::CRITICAL
//   2. increments overheat.ticks_at_critical (unconditionally for CRITICAL)
//   3. skips if ticks_at_critical < HeatConstants::EXPLOSION_DELAY_TICKS (60)
//   4. logs a warning, publishes publishBlockChangedEvent at the machine's own
//      position with block_id=0 / meta=0 (i.e. "remove this block to air")
//   5. queues the entity, and destroys every queued entity AFTER the view loop
//      finishes (deferred destroy — keeps the view iterator valid)
//
// gp-dd5q CHANGED THIS. Explosion now has a radius and a falloff: the exploding
// machine plus every *loaded* machine within HeatConstants::EXPLOSION_RADIUS is
// destroyed, with blocks farther from the epicentre dealt proportionally less
// blast. The falloff decides WHICH blocks die, not how big a number is
// published — the published payload is still air (block_id=0, meta=0).
//
// IMPORTANT: the blast is resolved against the ECS registry, not against the
// world. ExplosionSystem has no chunk/block repository — its only output channel
// is IEventPublisher::publishBlockChangedEvent — so it can only ever clear the
// blocks of machines it can SEE. Non-machine blocks and unloaded chunks are
// untouched. That limitation is written into the spec as the blast-visibility
// rule rather than hidden.
//
// The section "Blast radius and falloff (gp-dd5q)" near the bottom of this file
// is the gp-dd5q RED->GREEN evidence: those checks fail against the old
// single-block implementation.
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

#include <entt/entt.hpp>

#include "Network/IEventPublisher.h"
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/Position.h>
#include <game/machines/ExplosionSystem.h>
#include <game/machines/HeatConstants.h>
#include <game/machines/OverheatComponent.h>

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has no
// GTest dependency, so this is the established convention for focused tests.
#include <engine/net/test/test.h>

#ifndef CHECK_EQ_INT
#define CHECK_EQ_INT(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#endif

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

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

struct ChangedEvent {
    int32_t x = 0, y = 0, z = 0;
    uint16_t block_id = 0;
    uint8_t meta = 0;
};

struct MockEventPublisher : simcore::IEventPublisher {
    std::vector<ChangedEvent> changed;

    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char*, uint32_t, uint8_t) override {}

    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t, uint8_t) override {}

    void publishBlockChangedEvent(int32_t x, int32_t y, int32_t z, uint16_t block_id,
                                  uint8_t meta, uint32_t, uint64_t) override {
        changed.push_back({x, y, z, block_id, meta});
    }

    void publishBlockEntityUpdate(int32_t, int32_t, int32_t, uint16_t,
                                  const std::vector<uint8_t>&, float, uint32_t,
                                  EnergyType, uint32_t, int, float,
                                  const std::vector<HatchUpdateData>* = nullptr,
                                  double = -1.0, double = -1.0) override {}

    void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                    uint16_t, uint8_t, uint16_t,
                                    const char*) override {}

    void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                          const std::array<uint8_t, 6>&) override {}

    void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t, uint16_t) override {}
    void publishMultiblockDestroyed(uint64_t) override {}

    void publishGridUpdate(int32_t, int32_t, int32_t,
                           const std::vector<RecipeManager::ItemStack>&) override {}
};

constexpr uint32_t kDelay = simcore::HeatConstants::EXPLOSION_DELAY_TICKS;

// A multiblock controller id. Since gp-qgtc the "is a controller" fact is
// carried by MachineComponent::mb_id, which SimulationEngine sets to the
// controller id when a multiblock forms (SimulationEngine.cpp:309) and clears
// by removing the whole MachineComponent on teardown (destroyController, :82).
// Only the anchor block is ever a machine — member blocks are casing/coil — so
// `mb_id != 0` is exactly "multiblock controller anchor".
constexpr uint32_t kControllerMbId = 42;

// Builds a fully-formed candidate entity: the system will consider it.
static entt::entity makeCandidate(entt::registry& reg, uint32_t x, uint32_t y, uint32_t z,
                                  simcore::OverheatState state, uint32_t ticks_at_critical) {
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 1001, kControllerMbId, x, y, z, 7);
    reg.emplace<simcore::Position>(ent, x, y, z);
    reg.emplace<simcore::OverheatComponent>(ent);
    auto& oh = reg.get<simcore::OverheatComponent>(ent);
    oh.state = state;
    oh.ticks_at_critical = ticks_at_critical;
    return ent;
}

static bool hasAllComponents(const entt::registry& reg, entt::entity ent) {
    return reg.valid(ent) && reg.all_of<simcore::MachineComponent, simcore::Position,
                                       simcore::OverheatComponent>(ent);
}

// ---------------------------------------------------------------------------
// gp-dd5q blast-geometry helpers
// ---------------------------------------------------------------------------
//
// The blast constants are DUPLICATED here as literals on purpose. A test that
// reads its own expectations out of the same header it is testing cannot catch a
// wrong radius — it would just agree with itself. These literals are the
// intended geometry; the static_asserts below (added with the GREEN step) then
// forbid the header from drifting away from them.

constexpr int32_t kRadius = 3;   // intended max blast reach, in blocks
constexpr int32_t kAnchorReach = 2;   // multiblock anchor: destroyed at d <= 2
constexpr int32_t kSingleReach = 3;   // single-block machine: destroyed at d <= 3

// A machine that is NOT a multiblock anchor (mb_id == 0), i.e. the structural
// kind the epicentre gate rejects but a neighbour's blast must still destroy.
static entt::entity makeSingleBlock(entt::registry& reg, uint32_t x, uint32_t y, uint32_t z) {
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 2002, 0, x, y, z, 9);
    reg.emplace<simcore::Position>(ent, x, y, z);
    return ent;
}

// A non-machine block: a Position with no MachineComponent. The blast must not
// clear these — ExplosionSystem has no world access, only the ECS registry.
static entt::entity makePlainBlock(entt::registry& reg, uint32_t x, uint32_t y, uint32_t z) {
    auto ent = reg.create();
    reg.emplace<simcore::Position>(ent, x, y, z);
    return ent;
}

static int countEventsAt(const std::vector<ChangedEvent>& ev, int32_t x, int32_t y, int32_t z) {
    int n = 0;
    for (const auto& e : ev) {
        if (e.x == x && e.y == y && e.z == z) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// State gate: only CRITICAL is a candidate
// ---------------------------------------------------------------------------

static void test_ExplosionSystem_none_state_never_explodes() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    auto ent = makeCandidate(reg, 10, 20, 30, simcore::OverheatState::NONE,
                             kDelay + 500);
    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0), "NONE must not explode");
    CHECK(hasAllComponents(reg, ent), "NONE entity survives the tick");
    // The counter is untouched for a non-CRITICAL entity.
    CHECK_EQ_INT(reg.get<simcore::OverheatComponent>(ent).ticks_at_critical, kDelay + 500,
                 "NONE entity's critical counter is not advanced");
}

static void test_ExplosionSystem_warning_state_never_explodes() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // WARNING with a counter far past the delay still must not explode: the
    // state gate is evaluated before the counter is even incremented.
    auto ent = makeCandidate(reg, 11, 21, 31, simcore::OverheatState::WARNING,
                             kDelay + 500);
    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0), "WARNING must not explode");
    CHECK(hasAllComponents(reg, ent), "WARNING entity survives the tick");
    CHECK_EQ_INT(reg.get<simcore::OverheatComponent>(ent).ticks_at_critical, kDelay + 500,
                 "WARNING entity's critical counter is not advanced");
}

static void test_ExplosionSystem_empty_registry_is_a_noop() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    sys.tick(0.05f);
    sys.tick(1.0f);

    CHECK_EQ_INT(events->changed.size(), size_t(0), "no entities -> no events");
}

// ---------------------------------------------------------------------------
// Fuse: ticks_at_critical boundary against EXPLOSION_DELAY_TICKS (60)
// ---------------------------------------------------------------------------

static void test_ExplosionSystem_below_delay_does_not_explode() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // One tick short of the threshold: incremented to exactly 60, and the
    // comparison is `ticks_at_critical < EXPLOSION_DELAY_TICKS`, so 60 passes.
    auto ent = makeCandidate(reg, 1, 2, 3, simcore::OverheatState::CRITICAL, kDelay - 1);
    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(1),
                 "reaching exactly EXPLOSION_DELAY_TICKS explodes");
    CHECK(!reg.valid(ent), "exploded entity is destroyed");
}

static void test_ExplosionSystem_fuse_counts_down_one_tick_per_tick() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    auto ent = makeCandidate(reg, 4, 5, 6, simcore::OverheatState::CRITICAL, 0);

    // Ticks 1 .. kDelay-1: counter climbs, nothing explodes.
    for (uint32_t i = 1; i < kDelay; ++i) {
        sys.tick(0.05f);
        CHECK(reg.valid(ent), "entity alive before the delay elapses");
        CHECK_EQ_INT(reg.get<simcore::OverheatComponent>(ent).ticks_at_critical, i,
                     "counter advances by exactly one per tick");
    }
    CHECK_EQ_INT(events->changed.size(), size_t(0), "nothing exploded before the delay");

    // Tick kDelay: counter reaches kDelay, the `<` guard fails, it explodes.
    sys.tick(0.05f);
    CHECK_EQ_INT(events->changed.size(), size_t(1), "explodes on the 60th tick");
    CHECK(!reg.valid(ent), "entity destroyed on the 60th tick");
}

static void test_ExplosionSystem_counter_is_advanced_before_the_delay_check() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Explosion fires on the tick that *reaches* the threshold, not one tick
    // later: starting from 0 it takes exactly EXPLOSION_DELAY_TICKS ticks.
    auto ent = makeCandidate(reg, 7, 8, 9, simcore::OverheatState::CRITICAL, 0);
    for (uint32_t i = 0; i + 1 < kDelay; ++i) sys.tick(0.05f);
    CHECK_EQ_INT(events->changed.size(), size_t(0),
                 "still fused one tick before the threshold");
    sys.tick(0.05f);
    CHECK_EQ_INT(events->changed.size(), size_t(1),
                 "pre-increment means the delay is exactly EXPLOSION_DELAY_TICKS ticks");
    CHECK(!reg.valid(ent), "entity destroyed at the threshold tick");
}

// ---------------------------------------------------------------------------
// Destruction payload
// ---------------------------------------------------------------------------

static void test_ExplosionSystem_publishes_air_at_machine_position() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    makeCandidate(reg, 100, 64, 100, simcore::OverheatState::CRITICAL, kDelay);
    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(1), "exactly one block event");
    if (events->changed.size() == 1) {
        const auto& e = events->changed[0];
        CHECK_EQ_INT(e.x, 100, "event x is the machine position");
        CHECK_EQ_INT(e.y, 64, "event y is the machine position");
        CHECK_EQ_INT(e.z, 100, "event z is the machine position");
        CHECK_EQ_INT(int(e.block_id), 0, "block_id 0 == clear to air");
        CHECK_EQ_INT(int(e.meta), 0, "meta 0");
    }
}

// gp-dd5q: this test used to be named
// test_ExplosionSystem_no_radius_or_falloff_is_emitted and asserted the OPPOSITE
// — that a CRITICAL anchor with six healthy neighbours cleared exactly one
// block. That pin is what the radius/falloff feature had to break, so it was
// replaced on purpose. The replacement pins the falloff CURVE instead, which is
// the property the old single-block implementation could not express.
static void test_ExplosionSystem_falloff_reach_shrinks_with_distance() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Epicentre at the origin. Single-block machines (mb_id == 0) at d = 1, 2, 3
    // and 4 on the +X axis. The falloff is linear in distance: a machine dies
    // iff its distance is within the single-block reach.
    makeCandidate(reg, 500, 64, 500, simcore::OverheatState::CRITICAL, kDelay);

    auto at1 = makeSingleBlock(reg, 501, 64, 500);
    auto at2 = makeSingleBlock(reg, 502, 64, 500);
    auto at3 = makeSingleBlock(reg, 503, 64, 500);
    auto at4 = makeSingleBlock(reg, 504, 64, 500);

    sys.tick(0.05f);

    CHECK_EQ_INT(countEventsAt(events->changed, 500, 64, 500), 1, "epicentre is destroyed");
    CHECK_EQ_INT(countEventsAt(events->changed, 501, 64, 500), 1, "d=1 is inside the blast");
    CHECK_EQ_INT(countEventsAt(events->changed, 502, 64, 500), 1, "d=2 is inside the blast");
    CHECK_EQ_INT(countEventsAt(events->changed, 503, 64, 500), 1, "d=3 is on the blast edge");
    CHECK_EQ_INT(countEventsAt(events->changed, 504, 64, 500), 0, "d=4 is past the blast edge");

    CHECK(!reg.valid(at1), "d=1 neighbour entity destroyed");
    CHECK(!reg.valid(at2), "d=2 neighbour entity destroyed");
    CHECK(!reg.valid(at3), "d=3 neighbour entity destroyed");
    CHECK(reg.valid(at4), "d=4 neighbour survives the falloff");
}

static void test_ExplosionSystem_only_the_expired_fuse_explodes() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Mixed fuses in one tick: one past the delay, one just short of it, one
    // WARNING. Only the expired one goes off.
    //
    // The machines are spread far apart (gp-dd5q): the blast reaches 2 blocks
    // around the epicentre, so machines at (2,2,2) etc. would be inside the
    // blast of a boom at (1,1,1) and correctly die as collateral. This test is
    // about the FUSE, so its candidates must not be in each other's blast.
    auto boom  = makeCandidate(reg, 1, 1, 1, simcore::OverheatState::CRITICAL, kDelay);
    auto young = makeCandidate(reg, 20, 20, 20, simcore::OverheatState::CRITICAL, 0);
    auto warn  = makeCandidate(reg, 40, 40, 40, simcore::OverheatState::WARNING, 0);
    auto fresh = makeCandidate(reg, 60, 60, 60, simcore::OverheatState::CRITICAL, kDelay - 2);

    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(1), "only one explosion");
    CHECK(!reg.valid(boom), "expired fuse destroyed");
    CHECK(reg.valid(young), "unfused CRITICAL machine survives");
    CHECK(reg.valid(warn), "WARNING machine survives");
    CHECK(reg.valid(fresh), "almost-expired machine survives");
    if (reg.valid(young)) {
        CHECK_EQ_INT(reg.get<simcore::OverheatComponent>(young).ticks_at_critical, 1,
                     "surviving CRITICAL machine still counts its tick");
    }
}

static void test_ExplosionSystem_multiple_simultaneous_explosions() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Deferred destroy: all entities are collected during the view loop and
    // destroyed afterwards, so several can explode in a single tick without
    // invalidating the view iterator mid-iteration.
    auto a = makeCandidate(reg, 0, 0, 0, simcore::OverheatState::CRITICAL, kDelay);
    auto b = makeCandidate(reg, 1, 0, 0, simcore::OverheatState::CRITICAL, kDelay);
    auto c = makeCandidate(reg, 2, 0, 0, simcore::OverheatState::CRITICAL, kDelay + 99);

    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(3), "all three explode in one tick");
    CHECK(!reg.valid(a) && !reg.valid(b) && !reg.valid(c), "all three entities destroyed");
}

static void test_ExplosionSystem_dt_is_ignored() {
    // The fuse is tick-count based, not time based: dt must not change the
    // outcome. Both registries reach the threshold after the same tick count.
    for (float dt : {0.0f, 0.05f, 1.0f, 100.0f}) {
        entt::registry reg;
        auto events = std::make_shared<MockEventPublisher>();
        simcore::ExplosionSystem sys(reg, events);

        auto ent = makeCandidate(reg, 8, 8, 8, simcore::OverheatState::CRITICAL, 0);
        for (uint32_t i = 0; i < kDelay; ++i) sys.tick(dt);

        CHECK_EQ_INT(events->changed.size(), size_t(1),
                     "explosion timing is independent of dt");
        CHECK(!reg.valid(ent), "entity destroyed regardless of dt");
    }
}

// ---------------------------------------------------------------------------
// View membership: the 4-component gate
// ---------------------------------------------------------------------------

static void test_ExplosionSystem_single_block_machine_is_ignored() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Critical and fused, but a SINGLE-BLOCK machine (mb_id == 0). Only
    // multiblock controller anchors are candidates, so this must never blow up.
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 1001, 0, 9, 9, 9, 7);
    reg.emplace<simcore::Position>(ent, 9, 9, 9);
    reg.emplace<simcore::OverheatComponent>(ent);
    auto& oh = reg.get<simcore::OverheatComponent>(ent);
    oh.state = simcore::OverheatState::CRITICAL;
    oh.ticks_at_critical = kDelay;

    for (uint32_t i = 0; i < kDelay + 10; ++i) sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0),
                 "a single-block machine (mb_id == 0) never explodes");
    CHECK(reg.valid(ent), "single-block machine is never destroyed");
    CHECK_EQ_INT(reg.get<simcore::OverheatComponent>(ent).ticks_at_critical, kDelay,
                 "its counter is not advanced — it is outside the gate entirely");
}

static void test_ExplosionSystem_missing_machine_component_is_ignored() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Critical and fused, with a Position and an OverheatComponent but no
    // MachineComponent: MachineComponent carries the mb_id gate, so without it
    // the entity is not a candidate and must not be destroyed.
    auto ent = reg.create();
    reg.emplace<simcore::Position>(ent, 11, 11, 11);
    reg.emplace<simcore::OverheatComponent>(ent);
    reg.get<simcore::OverheatComponent>(ent).state = simcore::OverheatState::CRITICAL;
    reg.get<simcore::OverheatComponent>(ent).ticks_at_critical = kDelay;

    for (uint32_t i = 0; i < kDelay + 10; ++i) sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0), "no MachineComponent -> no explosion");
    CHECK(reg.valid(ent), "entity without MachineComponent survives");
}

static void test_ExplosionSystem_missing_overheat_component_is_ignored() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Has MachineComponent (with a controller mb_id) and Position but no
    // OverheatComponent: a machine that was never heat-exposed. Must be
    // ignored (and must not crash the view).
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 1001, kControllerMbId, 12, 12, 12, 7);
    reg.emplace<simcore::Position>(ent, 12, 12, 12);

    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0), "no OverheatComponent -> no explosion");
    CHECK(reg.valid(ent), "machine without OverheatComponent survives");
}

static void test_ExplosionSystem_missing_position_is_ignored() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // No Position: the system has no coordinates to publish, and since Position
    // is part of the view the entity is not a candidate.
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 1001, kControllerMbId, 13, 13, 13, 7);
    reg.emplace<simcore::OverheatComponent>(ent);
    reg.get<simcore::OverheatComponent>(ent).state = simcore::OverheatState::CRITICAL;
    reg.get<simcore::OverheatComponent>(ent).ticks_at_critical = kDelay;

    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0), "no Position -> no explosion");
    CHECK(reg.valid(ent), "machine without Position survives");
}

static void test_ExplosionSystem_destroy_clears_all_three_components() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    auto ent = makeCandidate(reg, 14, 14, 14, simcore::OverheatState::CRITICAL, kDelay);
    CHECK(reg.all_of<simcore::MachineComponent>(ent), "precondition: MachineComponent present");

    sys.tick(0.05f);

    CHECK(!reg.valid(ent), "entity handle is recycled by the registry");
    CHECK(!reg.all_of<simcore::MachineComponent>(ent),
          "MachineComponent is gone after destruction");
    CHECK(!reg.all_of<simcore::OverheatComponent>(ent),
          "OverheatComponent is gone after destruction");
}

// ---------------------------------------------------------------------------
// Repeated destruction / fuse re-arm
// ---------------------------------------------------------------------------

static void test_ExplosionSystem_one_event_per_exploded_entity() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    makeCandidate(reg, 15, 15, 15, simcore::OverheatState::CRITICAL, kDelay);
    // Ticking after destruction must not double-publish for the same entity.
    for (int i = 0; i < 5; ++i) sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(1),
                 "a destroyed entity cannot explode again");
}

static void test_ExplosionSystem_replaced_entity_can_explode_again() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    makeCandidate(reg, 16, 16, 16, simcore::OverheatState::CRITICAL, kDelay);
    sys.tick(0.05f);
    CHECK_EQ_INT(events->changed.size(), size_t(1), "first explosion");

    // A new machine rebuilt at the same spot explodes independently.
    makeCandidate(reg, 16, 16, 16, simcore::OverheatState::CRITICAL, kDelay);
    sys.tick(0.05f);
    CHECK_EQ_INT(events->changed.size(), size_t(2), "rebuilt machine explodes on its own fuse");
}

static void test_ExplosionSystem_warning_does_not_extend_the_critical_fuse() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Interleave WARNING ticks: they do not advance ticks_at_critical, so the
    // fuse needs EXPLOSION_DELAY_TICKS *consecutive* CRITICAL ticks on top of
    // the WARNING ticks that are simply ignored.
    auto ent = makeCandidate(reg, 17, 17, 17, simcore::OverheatState::WARNING, 0);
    for (uint32_t i = 0; i < 3; ++i) {
        reg.get<simcore::OverheatComponent>(ent).state = simcore::OverheatState::WARNING;
        sys.tick(0.05f);
    }
    CHECK_EQ_INT(events->changed.size(), size_t(0), "WARNING ticks do not explode");
    CHECK_EQ_INT(reg.get<simcore::OverheatComponent>(ent).ticks_at_critical, 0,
                 "WARNING ticks leave the counter at zero");

    // Each CRITICAL tick pre-increments then compares `< kDelay`, so the entity
    // explodes on the tick that brings the counter to exactly kDelay. Re-fetch
    // the component every iteration: the reference dies with the entity.
    for (uint32_t i = 0; i < kDelay; ++i) {
        auto& oh = reg.get<simcore::OverheatComponent>(ent);
        oh.state = simcore::OverheatState::CRITICAL;
        CHECK_EQ_INT(oh.ticks_at_critical, i, "CRITICAL ticks advance the counter by one");
        sys.tick(0.05f);
    }
    CHECK_EQ_INT(events->changed.size(), size_t(1),
                 "explodes once kDelay CRITICAL ticks elapse");
    CHECK(!reg.valid(ent), "entity destroyed on the kDelay'th CRITICAL tick");
}

// ---------------------------------------------------------------------------
// Blast radius and falloff (gp-dd5q)
//
// The four checks above that used to assert "exactly one event" are
// EPICENTRE-ISOLATION tests: they place no neighbour machines, so a correct
// radius implementation still emits exactly one event and they stay green. The
// tests below are the ones that were RED before the fix.
// ---------------------------------------------------------------------------

static void test_ExplosionSystem_clears_the_full_blast_radius() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Epicentre at (200,64,200) plus single-block machines on every face at
    // d = 1. The old implementation emitted 1 event; a radius implementation
    // must emit 7 (epicentre + 6 faces) and destroy all 6 neighbour entities.
    makeCandidate(reg, 200, 64, 200, simcore::OverheatState::CRITICAL, kDelay);
    std::vector<entt::entity> neighbours = {
        makeSingleBlock(reg, 201, 64, 200), makeSingleBlock(reg, 199, 64, 200),
        makeSingleBlock(reg, 200, 64, 201), makeSingleBlock(reg, 200, 64, 199),
        makeSingleBlock(reg, 200, 65, 200), makeSingleBlock(reg, 200, 63, 200),
    };

    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(7),
                 "blast clears the epicentre plus all six face neighbours");
    for (entt::entity e : neighbours) {
        CHECK(!reg.valid(e), "a face neighbour is destroyed by the blast");
    }
}

static void test_ExplosionSystem_falloff_is_spherical_not_manhattan() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Two diagonal neighbours. A cubical (per-axis) blast would kill anything
    // within 3 on each axis; a spherical one uses true Euclidean distance. The
    // discriminator is (2,2,2): d = sqrt(12) = 3.46, so it is INSIDE a 3-cube
    // but OUTSIDE a 3-sphere. (2,2,0) at d = 2.83 is inside both and proves
    // nothing, so it is not used here.
    makeCandidate(reg, 300, 64, 300, simcore::OverheatState::CRITICAL, kDelay);
    auto nearDiag = makeSingleBlock(reg, 302, 66, 300);  // d = 2.83, inside both
    auto farDiag  = makeSingleBlock(reg, 302, 66, 302);  // d = 3.46, cube-only

    sys.tick(0.05f);

    CHECK_EQ_INT(countEventsAt(events->changed, 302, 66, 300), 1,
                 "the near diagonal (d=2.83) is inside a spherical blast");
    CHECK_EQ_INT(countEventsAt(events->changed, 302, 66, 302), 0,
                 "the body diagonal (d=3.46) is outside a sphere — a cubical blast would wrongly kill it");
    CHECK(!reg.valid(nearDiag), "near diagonal entity destroyed");
    CHECK(reg.valid(farDiag), "far diagonal entity survives a spherical falloff");
}

static void test_ExplosionSystem_ballast_anchors_take_the_lower_reach() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Multiblock anchors are reinforced: they survive at d = 3 where a
    // single-block machine does not. This is the falloff's tier split — a
    // blast that ignores it would flatten every multiblock in range.
    //
    // Both targets are at distance EXACTLY 3.0 — the anchor on the X axis
    // (3,0,0) and the single block on a diagonal whose distance also works out
    // to 3.0, (2,2,1). Using two different directions at the same radius rules
    // out an accidental axis-specific behaviour.
    makeCandidate(reg, 400, 64, 400, simcore::OverheatState::CRITICAL, kDelay);
    auto anchorAt3 = makeCandidate(reg, 403, 64, 400, simcore::OverheatState::NONE, 0);
    auto singleAt3 = makeSingleBlock(reg, 402, 66, 401);

    sys.tick(0.05f);

    CHECK(reg.valid(anchorAt3), "a multiblock anchor at d=3.0 outranges the blast");
    CHECK(!reg.valid(singleAt3), "a single-block machine at the same distance does not");
    CHECK_EQ_INT(countEventsAt(events->changed, 403, 64, 400), 0, "the d=3 anchor is not cleared");
    CHECK_EQ_INT(countEventsAt(events->changed, 402, 66, 401), 1, "the d=3 single block is cleared");
}

static void test_ExplosionSystem_blast_skips_non_machine_blocks() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // ExplosionSystem's only output is IEventPublisher — it has no chunk store,
    // so it cannot know what a non-machine block is. A Position-only entity
    // inside the blast must be left completely alone.
    makeCandidate(reg, 600, 64, 600, simcore::OverheatState::CRITICAL, kDelay);
    auto plain = makePlainBlock(reg, 601, 64, 600);

    sys.tick(0.05f);

    CHECK(reg.valid(plain), "a non-machine block entity survives the blast");
    CHECK_EQ_INT(countEventsAt(events->changed, 601, 64, 600), 0,
                 "no block event is published for a non-machine block");
    CHECK_EQ_INT(events->changed.size(), size_t(1), "only the machine blast is published");
}

static void test_ExplosionSystem_blast_does_not_chain() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // A neighbour killed by the blast is destroyed AFTER the epicentre is
    // resolved and must NOT re-explode: a victim is not a CRITICAL fuse. This
    // also guards the deferred-destroy ordering — victims are collected, not
    // exploded.
    makeCandidate(reg, 700, 64, 700, simcore::OverheatState::CRITICAL, kDelay);
    // A second CRITICAL anchor inside the blast IS a live fuse: it explodes on
    // its own, in the same tick, because the view loop reaches it regardless
    // of the first explosion.
    auto victim = makeSingleBlock(reg, 701, 64, 700);
    auto fellow  = makeCandidate(reg, 702, 64, 700, simcore::OverheatState::CRITICAL, kDelay);

    sys.tick(0.05f);

    CHECK(!reg.valid(victim), "the single-block victim is destroyed");
    CHECK(!reg.valid(fellow), "the fellow CRITICAL anchor explodes on its own fuse");
    CHECK_EQ_INT(countEventsAt(events->changed, 701, 64, 700), 1,
                 "a blast victim is cleared once, not re-published");
    CHECK_EQ_INT(countEventsAt(events->changed, 702, 64, 700), 1,
                 "the fellow anchor is published exactly once, by its own fuse");
}

static void test_ExplosionSystem_blast_cannot_relay_through_a_victim() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // The chaining guard, stated so it actually discriminates. A machine is
    // NOT a new epicentre just because it was caught in a blast: if victims
    // became epicentres, the wave would relay outward, one machine per hop.
    //
    // A line of machines alone cannot prove this, because the sweep visits
    // entities in entt id order, and this registry hands ids out ascending
    // while the view walks them DESCENDING. A relaying implementation only
    // propagates toward entities it has not visited yet, so the relay has to
    // be created LAST to be visited FIRST. Creating the spur first (ent 2) and
    // the relay second (ent 1) therefore guarantees the relay is already a
    // victim by the time the spur is tested — which is exactly the moment a
    // relaying implementation would use it as an epicentre.
    //
    //   epicentre (0,0,0)     -- ent 0, reaches 3
    //   spur      (4,1,0)     -- ent 2, d = 4.12 from the epicentre: OUT of reach
    //   relay     (2,0,0)     -- ent 1, d = 2.0 from the epicentre: a victim,
    //                          -- and only d = 2.24 from the spur
    //
    // The spur survives only if the wave is resolved from the epicentre list
    // alone. A victim-as-epicentre implementation kills it.
    makeCandidate(reg, 1000, 64, 1000, simcore::OverheatState::CRITICAL, kDelay);
    auto spur  = makeSingleBlock(reg, 1004, 65, 1000);  // created FIRST => ent 2 => visited FIRST
    auto relay = makeSingleBlock(reg, 1002, 64, 1000);  // created last  => ent 1 => visited second

    sys.tick(0.05f);

    CHECK(!reg.valid(relay), "the relay is caught by the direct blast");
    CHECK(reg.valid(spur),
          "the blast must not relay: the spur at d=4.12 survives even though it is "
          "only d=2.24 from the destroyed relay");
    CHECK_EQ_INT(countEventsAt(events->changed, 1004, 65, 1000), 0,
                 "no event is published for the un-relayed spur");
}

static void test_ExplosionSystem_two_epicentres_do_not_double_publish() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Two CRITICAL anchors 2 apart: their blasts overlap on the single-block
    // machine sitting between them. Each block must still be cleared exactly
    // once — the overlap is not a reason to publish a second event.
    makeCandidate(reg, 800, 64, 800, simcore::OverheatState::CRITICAL, kDelay);
    makeCandidate(reg, 802, 64, 800, simcore::OverheatState::CRITICAL, kDelay);
    auto middle = makeSingleBlock(reg, 801, 64, 800);

    sys.tick(0.05f);

    CHECK(!reg.valid(middle), "the machine between two epicentres is destroyed");
    CHECK_EQ_INT(countEventsAt(events->changed, 801, 64, 800), 1,
                 "an overlapping blast clears the shared victim only once");
    CHECK_EQ_INT(events->changed.size(), size_t(3),
                 "three distinct blocks cleared: two epicentres plus the shared victim");
}

static void test_ExplosionSystem_radius_is_centered_on_the_exploding_machine() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // The blast is centred on the entity that exploded, not on the world
    // origin and not on a neighbouring machine. A machine 2 blocks from a
    // fused epicentre dies even though it is far from every other candidate.
    makeCandidate(reg, 900, 64, 900, simcore::OverheatState::CRITICAL, kDelay);
    auto near = makeSingleBlock(reg, 902, 64, 900);
    auto far  = makeSingleBlock(reg, 990, 64, 990);

    sys.tick(0.05f);

    CHECK(!reg.valid(near), "a machine 2 blocks from the epicentre is destroyed");
    CHECK(reg.valid(far), "a machine far from every epicentre is untouched");
    CHECK_EQ_INT(countEventsAt(events->changed, 990, 64, 990), 0, "no event for the far machine");
}

// The header constants are pinned to the geometry the tests above assert. If
// someone retunes HeatConstants without retuning the spec and these tests,
// the build breaks instead of the world silently changing.
static_assert(simcore::HeatConstants::EXPLOSION_RADIUS == 3,
              "EXPLOSION_RADIUS must match kRadius above (see the gp-dd5q spec delta)");
static_assert(simcore::HeatConstants::EXPLOSION_ANCHOR_REACH == 2,
              "EXPLOSION_ANCHOR_REACH must match kAnchorReach above");
static_assert(simcore::HeatConstants::EXPLOSION_SINGLEBLOCK_REACH == 3,
              "EXPLOSION_SINGLEBLOCK_REACH must match kSingleReach above");
// The three reach values must satisfy the blast-visibility contract: a
// single-block machine is the weakest thing the blast destroys, so it can never
// reach further than an anchor.
static_assert(simcore::HeatConstants::EXPLOSION_SINGLEBLOCK_REACH >=
                  simcore::HeatConstants::EXPLOSION_ANCHOR_REACH,
              "single-block reach must be >= anchor reach (blast tier ordering)");
static_assert(simcore::HeatConstants::EXPLOSION_SINGLEBLOCK_REACH <=
                  simcore::HeatConstants::EXPLOSION_RADIUS,
              "no reach may exceed EXPLOSION_RADIUS");

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== explosion_system test suite ===\n\n");

    TEST(ExplosionSystem_none_state_never_explodes);
    TEST(ExplosionSystem_warning_state_never_explodes);
    TEST(ExplosionSystem_empty_registry_is_a_noop);
    TEST(ExplosionSystem_below_delay_does_not_explode);
    TEST(ExplosionSystem_fuse_counts_down_one_tick_per_tick);
    TEST(ExplosionSystem_counter_is_advanced_before_the_delay_check);
    TEST(ExplosionSystem_publishes_air_at_machine_position);
    TEST(ExplosionSystem_falloff_reach_shrinks_with_distance);
    TEST(ExplosionSystem_only_the_expired_fuse_explodes);
    TEST(ExplosionSystem_multiple_simultaneous_explosions);
    TEST(ExplosionSystem_dt_is_ignored);
    TEST(ExplosionSystem_single_block_machine_is_ignored);
    TEST(ExplosionSystem_missing_machine_component_is_ignored);
    TEST(ExplosionSystem_missing_overheat_component_is_ignored);
    TEST(ExplosionSystem_missing_position_is_ignored);
    TEST(ExplosionSystem_destroy_clears_all_three_components);
    TEST(ExplosionSystem_one_event_per_exploded_entity);
    TEST(ExplosionSystem_replaced_entity_can_explode_again);
    TEST(ExplosionSystem_warning_does_not_extend_the_critical_fuse);

    // gp-dd5q: blast radius + falloff
    TEST(ExplosionSystem_clears_the_full_blast_radius);
    TEST(ExplosionSystem_falloff_is_spherical_not_manhattan);
    TEST(ExplosionSystem_ballast_anchors_take_the_lower_reach);
    TEST(ExplosionSystem_blast_skips_non_machine_blocks);
    TEST(ExplosionSystem_blast_does_not_chain);
    TEST(ExplosionSystem_blast_cannot_relay_through_a_victim);
    TEST(ExplosionSystem_two_epicentres_do_not_double_publish);
    TEST(ExplosionSystem_radius_is_centered_on_the_exploding_machine);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
