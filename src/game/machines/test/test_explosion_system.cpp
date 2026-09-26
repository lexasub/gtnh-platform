// ExplosionSystem unit tests (issue gp-43vj).
//
// Covers src/game/machines/ExplosionSystem.cpp — the CRITICAL-overheat fuse and
// the block destruction it performs.
//
// The system view is:
//     reg_.view<MachineComponent, Position, OverheatComponent, MultiblockController>()
// An entity is therefore only a candidate if it carries ALL FOUR components.
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
// NOTE: there is no radius, no falloff, and no damage scaling anywhere in this
// file. An explosion affects exactly one block: the exploding machine's own
// position. See test_ExplosionSystem_no_radius_or_falloff_is_emitted below, which
// pins that so a future radius/falloff feature has to update this test on purpose.
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

#include <entt/entt.hpp>

#include "Network/IEventPublisher.h"
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/MultiblockController.h>
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

// A controller with no blocks. Spelled out rather than `{}` so the 6-arg
// MultiblockController constructor is unambiguously selected over the default one.
static simcore::MultiblockController makeController(uint64_t id, uint32_t x, uint32_t y,
                                                    uint32_t z, uint32_t pattern_id) {
    return simcore::MultiblockController(id, x, y, z, pattern_id,
                                         std::vector<uint32_t>{});
}

// Builds a fully-formed candidate entity: the system will consider it.
static entt::entity makeCandidate(entt::registry& reg, uint32_t x, uint32_t y, uint32_t z,
                                  simcore::OverheatState state, uint32_t ticks_at_critical) {
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 1001, 0, x, y, z, 7);
    reg.emplace<simcore::Position>(ent, x, y, z);
    reg.emplace<simcore::OverheatComponent>(ent);
    auto& oh = reg.get<simcore::OverheatComponent>(ent);
    oh.state = state;
    oh.ticks_at_critical = ticks_at_critical;
    reg.emplace<simcore::MultiblockController>(ent, makeController(42, x, y, z, 1));
    return ent;
}

static bool hasAllComponents(const entt::registry& reg, entt::entity ent) {
    return reg.valid(ent) && reg.all_of<simcore::MachineComponent, simcore::Position,
                                       simcore::OverheatComponent,
                                       simcore::MultiblockController>(ent);
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

static void test_ExplosionSystem_no_radius_or_falloff_is_emitted() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Neighbouring machines on all six faces, all healthy. The system has no
    // radius/falloff/damage concept, so none of them may be touched: exactly
    // one block event, for the exploding machine only.
    makeCandidate(reg, 500, 64, 500, simcore::OverheatState::CRITICAL, kDelay);
    auto north = makeCandidate(reg, 501, 64, 500, simcore::OverheatState::NONE, 0);
    auto south = makeCandidate(reg, 499, 64, 500, simcore::OverheatState::NONE, 0);
    auto east  = makeCandidate(reg, 500, 64, 501, simcore::OverheatState::NONE, 0);
    auto west  = makeCandidate(reg, 500, 64, 499, simcore::OverheatState::NONE, 0);
    auto above = makeCandidate(reg, 500, 65, 500, simcore::OverheatState::NONE, 0);
    auto below = makeCandidate(reg, 500, 63, 500, simcore::OverheatState::NONE, 0);

    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(1),
                 "explosion destroys exactly one block — no radius, no falloff");
    if (events->changed.size() == 1) {
        CHECK_EQ_INT(events->changed[0].x, 500, "the event targets the exploding machine");
        CHECK_EQ_INT(events->changed[0].y, 64, "the event targets the exploding machine");
        CHECK_EQ_INT(events->changed[0].z, 500, "the event targets the exploding machine");
    }
    for (entt::entity e : {north, south, east, west, above, below}) {
        CHECK(reg.valid(e), "adjacent machine is not destroyed by a neighbour's explosion");
    }
}

static void test_ExplosionSystem_only_the_expired_fuse_explodes() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Mixed fuses in one tick: one past the delay, one just short of it, one
    // WARNING. Only the expired one goes off.
    auto boom  = makeCandidate(reg, 1, 1, 1, simcore::OverheatState::CRITICAL, kDelay);
    auto young = makeCandidate(reg, 2, 2, 2, simcore::OverheatState::CRITICAL, 0);
    auto warn  = makeCandidate(reg, 3, 3, 3, simcore::OverheatState::WARNING, 0);
    auto fresh = makeCandidate(reg, 4, 4, 4, simcore::OverheatState::CRITICAL, kDelay - 2);

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

static void test_ExplosionSystem_missing_multiblock_controller_is_ignored() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Critical, fused, but WITHOUT MultiblockController — the 4-type view does
    // not match, so the system must not see it at all.
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 1001, 0, 9, 9, 9, 7);
    reg.emplace<simcore::Position>(ent, 9, 9, 9);
    reg.emplace<simcore::OverheatComponent>(ent);
    auto& oh = reg.get<simcore::OverheatComponent>(ent);
    oh.state = simcore::OverheatState::CRITICAL;
    oh.ticks_at_critical = kDelay;

    for (uint32_t i = 0; i < kDelay + 10; ++i) sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0),
                 "entity without MultiblockController is outside the view");
    CHECK(reg.valid(ent), "entity without MultiblockController is never destroyed");
    CHECK_EQ_INT(reg.get<simcore::OverheatComponent>(ent).ticks_at_critical, kDelay,
                 "out-of-view entity's counter is not advanced");
}

static void test_ExplosionSystem_missing_overheat_component_is_ignored() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    simcore::ExplosionSystem sys(reg, events);

    // Has the other three components but no OverheatComponent: machine that was
    // never heat-exposed. Must be ignored (and must not crash the view).
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 1001, 0, 12, 12, 12, 7);
    reg.emplace<simcore::Position>(ent, 12, 12, 12);
    reg.emplace<simcore::MultiblockController>(ent, makeController(43, 12, 12, 12, 1));

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
    reg.emplace<simcore::MachineComponent>(ent, 1001, 0, 13, 13, 13, 7);
    reg.emplace<simcore::OverheatComponent>(ent);
    reg.get<simcore::OverheatComponent>(ent).state = simcore::OverheatState::CRITICAL;
    reg.get<simcore::OverheatComponent>(ent).ticks_at_critical = kDelay;
    reg.emplace<simcore::MultiblockController>(ent, makeController(44, 13, 13, 13, 1));

    sys.tick(0.05f);

    CHECK_EQ_INT(events->changed.size(), size_t(0), "no Position -> no explosion");
    CHECK(reg.valid(ent), "machine without Position survives");
}

static void test_ExplosionSystem_destroy_clears_all_four_components() {
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
    CHECK(!reg.all_of<simcore::MultiblockController>(ent),
          "MultiblockController is gone after destruction");
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
    TEST(ExplosionSystem_no_radius_or_falloff_is_emitted);
    TEST(ExplosionSystem_only_the_expired_fuse_explodes);
    TEST(ExplosionSystem_multiple_simultaneous_explosions);
    TEST(ExplosionSystem_dt_is_ignored);
    TEST(ExplosionSystem_missing_multiblock_controller_is_ignored);
    TEST(ExplosionSystem_missing_overheat_component_is_ignored);
    TEST(ExplosionSystem_missing_position_is_ignored);
    TEST(ExplosionSystem_destroy_clears_all_four_components);
    TEST(ExplosionSystem_one_event_per_exploded_entity);
    TEST(ExplosionSystem_replaced_entity_can_explode_again);
    TEST(ExplosionSystem_warning_does_not_extend_the_critical_fuse);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
