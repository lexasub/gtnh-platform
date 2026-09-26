// GeneratorSystem unit tests (issue gp-lqj).
//
// Covers src/game/machines/GeneratorSystem.cpp — the solid-fuel burn path of
// the two fuel generators, and specifically the removal of the invented
// `maxOutput = 32` / `capacity = 10000` fallback that used to sit between the
// fuel scan and the produceEnergy() call.
//
// DRIVER SHAPE (why these tests look the way they do):
//   GeneratorSystem::tick DOES iterate an EnTT view:
//     reg_.view<MachineComponent, InventoryContainer, EnergyStorage>()   (:33)
//   so there is no controllers_ map to supply — the test just places a
//   component triple on an entity and ticks. It is NOT dead code: main.cpp
//   registers it unconditionally alongside the boiler/EBF/LCR systems, and
//   SimulationEngine.cpp:219-259 gives every machine entity exactly this
//   triple. That is also where the test gets its values — see below.
//
// THE REAL SOURCE OF capacity / maxOutput (the whole point of gp-lqj):
//   SimulationEngine.cpp:241-244 copies MachineInfo::capacity,
//   ::maxInput and ::maxOutput out of machines.yaml into the EnergyStorage
//   that GeneratorSystem reads. For the coal generator
//   (machines.yaml:180-187, 1110:000:2) that is capacity 10000, max_output
//   32; for the solid steam boiler (:156-163, 1110:011:0) capacity 10000,
//   max_output 32. So the component values ARE the content data, and the
//   fallback had nothing to add — it only ever papered over a zero that meant
//   "this generator has no declared buffer", and then published 10000 as
//   fact to the client.
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed" here):
//   A. A GENERATOR WITH A DECLARED BUFFER BURNS NORMALLY AND THE BURN RATE
//      IS THE DECLARED max_output, not a constant. Pinned by
//      test_GeneratorSystem_declared_max_output_is_the_burn_rate.
//   B. THE FALLBACK IS GONE: a generator whose EnergyStorage declares
//      capacity 0 produces NOTHING, publishes no block-entity update, and —
//      critically — DOES NOT CONSUME FUEL. The old code burned the coal first
//      and only then discovered the buffer was 0, and the invented 10000 then
//      let produceEnergy() charge a buffer the content data never declared.
//      Pinned by test_GeneratorSystem_zero_capacity_burns_nothing_and_keeps_its_fuel
//      and test_GeneratorSystem_zero_capacity_publishes_no_invented_bar.
//   B2. isFull() IS `current >= capacity` (EnergyStorage.h:47), so a
//      capacity-0 generator is trivially "full" at ANY charge level. The gate
//      therefore sits ABOVE the isFull() check: placed below it, a capacity-0
//      generator would return early and sit silently idle forever — the same
//      invisibility the fallback hid, only quieter, with no log line pointing
//      at it. Pinned by
//      test_GeneratorSystem_zero_capacity_at_zero_charge_is_refused_not_treated_as_full.
//   B3. CONSEQUENCE OF B2, and the precise reach of the removed fallback: the
//      old order was isFull() first, fallback second, so the invented 10000
//      could only fire when current < capacity <= 0 — i.e. only for a
//      generator holding NEGATIVE energy. For every other capacity-0 case the
//      fallback was already unreachable and the machine was merely parked.
//      That is why the capacity tests use a non-zero charge, and why the one
//      test that must show the old behaviour is the negative-charge case.
//      Pinned by
//      test_GeneratorSystem_zero_capacity_with_negative_charge_is_the_reachable_fallback_case,
//      which fails 4 assertions against the old code.
//   C. THE SAME RULE GOVERNS max_output: a generator with capacity but
//      max_output 0 is equally misconfigured and is refused.
//      Pinned by test_GeneratorSystem_zero_max_output_is_refused.
//   D. THE REFUSAL IS PER-TICK AND DOES NOT GO STICKY. Once the content data
//      is corrected the very next tick charges normally — the system holds no
//      latch, so an operator who fixes machines.yaml is unblocked without a
//      restart. Pinned by
//      test_GeneratorSystem_recovering_capacity_starts_charging_next_tick.
//   E. A GENERATOR AT ZERO FUEL IS UNAFFECTED BY THE CHECK: the gate is
//      after isFull()/before the scan, so an empty but well-configured
//      generator is simply idle, not an error. Pinned by
//      test_GeneratorSystem_well_configured_empty_generator_is_quiet.
//   F. THE STEAM-ID GATE STILL WINS. A STEAM generator with steam_id 0 is
//      skipped at the top of the tick, before the new capacity check, so it
//      never logs the capacity error and never burns. Pinned by
//      test_GeneratorSystem_steam_gate_precedes_the_capacity_check.
//   G. THE BLOCK-ENTITY UPDATE PUBLISHES energy.capacity, the same field the
//      old fallback used to fabricate — see the contrast note in gp-bbbl,
//      where RotareGeneratorSystem publishes 0 and the client falls back to
//      tier*10000. Pinned by
//      test_GeneratorSystem_publishes_the_real_capacity.
//
// DETERMINISM: no wall clock, no network, no cluster, no display. The
// publisher is a recording double and the PipeEnergyClient / FluidClient are
// nullptr, which every publish call site in the system guards. tick() is
// driven by an explicit tick count, never by a sleep.
#include <cstdio>
#include <cstdint>
#include <array>
#include <memory>
#include <vector>

#include <entt/entt.hpp>

#include "Network/IEventPublisher.h"
#include <content/content.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <game/machines/GeneratorSystem.h>

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has
// no GTest dependency, so this is the established convention for focused tests.
#include <engine/net/test/test.h>

#ifndef CHECK_EQ_INT
#define CHECK_EQ_INT(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#endif
#ifndef CHECK_GT_INT
#define CHECK_GT_INT(a, b, ...) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, ##__VA_ARGS__)
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
// Doubles
// ---------------------------------------------------------------------------

struct PublisherEvent {
    int32_t x = 0, y = 0, z = 0;
    uint16_t machine_id = 0;
    float progress = 0.0f;
    uint32_t energy = 0;
    uint32_t energy_capacity = 0;
    EnergyType energy_type = EnergyType::ELECTRICITY;
};

struct RecordingPublisher : simcore::IEventPublisher {
    std::vector<PublisherEvent> updates;

    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char*, uint32_t, uint8_t) override {}

    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t, uint8_t) override {}

    void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                  uint32_t, uint64_t) override {}

    void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z, uint16_t machine_type,
                                  const std::vector<uint8_t>&, float progress,
                                  uint32_t energy, EnergyType energy_type,
                                  uint32_t energy_capacity, int, float,
                                  const std::vector<HatchUpdateData>* = nullptr,
                                  double = -1.0, double = -1.0) override {
        updates.push_back({x, y, z, machine_type, progress, energy, energy_capacity,
                           energy_type});
    }

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

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// Null pipe/fluid clients: GeneratorSystem guards every publishNodeUpdate with
// `if (pipeClient_)` / `if (fluidClient_)`, so the whole burn path runs with no
// router, no socket and no wall clock. The steam id is passed explicitly (0 by
// default) because that is the fail-closed gate at the top of the tick.
struct Fixture {
    entt::registry reg;
    std::shared_ptr<RecordingPublisher> events = std::make_shared<RecordingPublisher>();
    // Declared AFTER events (and destroyed before it) because GeneratorSystem
    // holds references to both reg and the publisher.
    std::unique_ptr<simcore::GeneratorSystem> sys;
};

// machines.yaml:180-187 — heat_generator (the coal fuel generator).
constexpr uint16_t kCoalGeneratorId = ItemId::pack("1110:000:2");
constexpr int32_t kDeclaredCapacity = 10000;
constexpr int32_t kDeclaredMaxOutput = 32;
constexpr int32_t kDeclaredTier = 0;

// machines.yaml:156-163 — steam_solid_boiler.
constexpr uint16_t kSteamBoilerId = ItemId::pack("1110:011:0");

// content.cpp:10 — coal is worth 8000 HU, so a single coal is enough fuel for
// many ticks and the burn never starves mid-test.
constexpr uint16_t kCoal = ItemId::pack("0:11110:2");
constexpr int32_t kCoalEnergy = 8000;

// A block id that is neither a generator nor anything else, used to prove the
// machine_id gate rather than the capacity gate is what skips it.
constexpr uint16_t kNotAGenerator = 0x0ACE;

static std::unique_ptr<Fixture> makeFixture(uint16_t steam_item_id = 0) {
    // MachineRegistry has a private constructor, so LoadFromYaml on an
    // unopenable path is the only way to get an EMPTY but non-null registry.
    // It is required, not optional: the slot-count lookup at
    // GeneratorSystem.cpp:216 calls `MachineRegistry::instance()->Get(...)`
    // WITHOUT a null guard on the singleton (pinned as a finding by
    // test_EBFSystem_null_machine_registry_is_a_missing_singleton_not_a_guarded_path).
    // That path is reached for every generator that actually burns, so a null
    // instance segfaults before the capacity check is even consulted. The new
    // check in this suite DOES guard the singleton; this pre-existing one does
    // not, and the fixture works around it the same way the EBF/LCR suites do.
    auto reg = MachineRegistry::LoadFromYaml("/nonexistent/machines.yaml");
    if (reg) MachineRegistry::setInstance(reg.release());

    auto f = std::make_unique<Fixture>();
    f->sys = std::make_unique<simcore::GeneratorSystem>(f->reg, f->events,
                                                        nullptr /* PipeEnergyClient */,
                                                        nullptr /* FluidClient */,
                                                        steam_item_id);
    return f;
}

// Places the exact component triple SimulationEngine.cpp:219-259 builds for a
// machine, with the capacity/max_output the caller names. Naming them as
// parameters is the point: the tests below drive deliberately bad content
// values through the same door production uses.
static entt::entity makeGenerator(entt::registry& reg,
                                  uint16_t machine_id,
                                  int32_t capacity,
                                  int32_t max_output,
                                  uint16_t fuel_id = kCoal,
                                  uint8_t fuel_count = 1,
                                  int32_t current = 0,
                                  EnergyType type = EnergyType::HEAT,
                                  int32_t max_input = kDeclaredMaxOutput) {
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, machine_id, 0, 10, 20, 30, 1);
    simcore::InventoryContainer container;
    container.entity_type = 1;
    container.slot_count = 1;
    container.slots.resize(1);
    container.slots[0].item_id = fuel_id;
    container.slots[0].count = fuel_count;
    reg.emplace<simcore::InventoryContainer>(ent, std::move(container));
    reg.emplace<simcore::EnergyStorage>(ent, capacity, current, max_input, max_output,
                                        kDeclaredTier, type);
    return ent;
}

static simcore::EnergyStorage& energy(entt::registry& reg, entt::entity ent) {
    return reg.get<simcore::EnergyStorage>(ent);
}

static const simcore::InventoryContainer& container(entt::registry& reg, entt::entity ent) {
    return reg.get<simcore::InventoryContainer>(ent);
}

static const PublisherEvent* lastUpdate(const Fixture& f) {
    return f.events->updates.empty() ? nullptr : &f.events->updates.back();
}

// ---------------------------------------------------------------------------
// A. The declared rate is the burn rate
// ---------------------------------------------------------------------------

static void test_GeneratorSystem_declared_constants() {
    // The two block ids the system recognises (content.h:12-13), asserted so a
    // rename in content shows up here rather than as a silently dead system.
    CHECK_EQ_INT(content::kGeneratorMachineId_Coal, kCoalGeneratorId,
                 "the coal generator id is the machines.yaml heat_generator");
    CHECK_EQ_INT(content::kGeneratorMachineId_Steam, kSteamBoilerId,
                 "the steam generator id is the machines.yaml steam_solid_boiler");
    CHECK_EQ_INT(simcore::GeneratorSystem::FuelValues().at(kCoal), kCoalEnergy,
                 "coal is worth 8000 HU (content.cpp:10)");
}

static void test_GeneratorSystem_declared_max_output_is_the_burn_rate() {
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, kDeclaredCapacity, kDeclaredMaxOutput);

    // One tick must charge EXACTLY max_output, which is the machines.yaml
    // value. The old code produced min(32, remaining) from a hardcoded 32; the
    // test that would have caught a mismatch is the next one.
    f->sys->tick(0.05f);
    CHECK_EQ_INT(energy(f->reg, ent).current, kDeclaredMaxOutput,
                 "one tick charges exactly the declared max_output (32)");

    // A DIFFERENT declared rate must be honoured too — that is what proves the
    // number came from the component and not from a constant baked into the
    // system. 5000/64 is the rotare_generator's pair (machines.yaml:206-207);
    // any positive pair works, the point is that it is not 32.
    auto f2 = makeFixture();
    auto ent2 = makeGenerator(f2->reg, kCoalGeneratorId, 5000, 64);
    f2->sys->tick(0.05f);
    CHECK_EQ_INT(energy(f2->reg, ent2).current, 64,
                 "a different declared max_output (64) is honoured, not clamped to 32");
}

static void test_GeneratorSystem_production_is_clamped_by_the_declared_capacity() {
    // capacity 40 with max_output 32: two ticks fit (32 + 8), the third cannot.
    // Proves capacity is likewise the declared number and not a 10000 constant.
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, 40, kDeclaredMaxOutput);

    f->sys->tick(0.05f);
    f->sys->tick(0.05f);
    CHECK_EQ_INT(energy(f->reg, ent).current, 40,
                 "the buffer stops at the declared capacity (40), not 10000");
    f->sys->tick(0.05f);
    CHECK_EQ_INT(energy(f->reg, ent).current, 40,
                 "a full generator stays full (isFull) rather than overcharging");
}

// ---------------------------------------------------------------------------
// B. The removed fallback: zero capacity burns nothing
// ---------------------------------------------------------------------------

static void test_GeneratorSystem_zero_capacity_burns_nothing_and_keeps_its_fuel() {
    auto f = makeFixture();
    // current 5 so the entity is NOT trivially full (see the note in
    // test_GeneratorSystem_zero_capacity_publishes_no_invented_bar).
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, /*capacity=*/0, kDeclaredMaxOutput,
                             kCoal, /*fuel_count=*/4, /*current=*/5);

    f->sys->tick(0.05f);
    f->sys->tick(0.05f);
    f->sys->tick(0.05f);

    CHECK_EQ_INT(energy(f->reg, ent).current, 5,
                 "a generator with no declared capacity produces nothing");
    // The invariant that matters. Under the old code the fallback assigned
    // capacity 10000, which made produceEnergy() charge happily: the machine
    // filled a 10000 HU buffer the content data never declared, and the
    // client drew a bar for it. Nothing may be invented here, and above all
    // the coal must survive.
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 4,
                 "the coal is NOT consumed: a generator that cannot charge must not eat fuel");
}

static void test_GeneratorSystem_zero_capacity_publishes_no_invented_bar() {
    auto f = makeFixture();
    // capacity 0 with a NON-zero stored charge is the only way to reach the
    // capacity gate with capacity 0: `isFull()` is `current >= capacity`
    // (EnergyStorage.h:47), so a 0/0 generator is trivially "full" and is
    // skipped at GeneratorSystem.cpp:76 before the gate is consulted. A
    // component that still holds charge but declares no capacity is the
    // reachable corruption case — and it is exactly what the old fallback
    // silently repaired into a 10000 HU buffer.
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, /*capacity=*/0, kDeclaredMaxOutput,
                             kCoal, /*fuel_count=*/1, /*current=*/5);
    f->sys->tick(0.05f);

    // The old code wrote 10000 into energy.capacity and then published it at
    // line 206, so the client drew a 10000 HU bar for a machine whose content
    // data declares no buffer at all. Refusing the tick publishes nothing,
    // which is the honest outcome — a misconfiguration is not a machine state.
    CHECK(f->events->updates.empty(),
          "a misconfigured generator publishes no block-entity update at all");
    CHECK_EQ_INT(energy(f->reg, ent).capacity, 0,
                 "and the component capacity is left at the declared 0, not rewritten to 10000");
}

static void test_GeneratorSystem_zero_capacity_at_zero_charge_is_refused_not_treated_as_full() {
    // capacity 0 AND current 0. This is the case that forced the gate ABOVE
    // the isFull() check: isFull() is `current >= capacity` (EnergyStorage.h:47),
    // so a 0/0 generator is trivially "full". Had the check stayed below
    // isFull() it would have returned at GeneratorSystem.cpp:76 and the
    // generator would sit silently idle forever — the same invisibility the
    // fallback was hiding, only quieter, with no log line to point at it. The
    // gate is now above, so this generator is refused visibly.
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, /*capacity=*/0, kDeclaredMaxOutput,
                             kCoal, /*fuel_count=*/1, /*current=*/0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(energy(f->reg, ent).current, 0, "capacity 0 at zero charge stays at zero");
    CHECK_EQ_INT(energy(f->reg, ent).capacity, 0,
                 "the 10000 fallback never gets a chance to run");
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 1, "no fuel is spent");
    CHECK(f->events->updates.empty(),
          "a capacity-0 generator publishes nothing, not a fabricated 10000 bar");
}

static void test_GeneratorSystem_zero_capacity_with_negative_charge_is_the_reachable_fallback_case() {
    // THE case the removed fallback actually governed, and the reason the
    // capacity-zero tests above use a POSITIVE stored charge.
    //
    // The old sequence was: `if (energy.isFull()) continue;` (current >=
    // capacity) and only then `capacity = capacity > 0 ? capacity : 10000`.
    // For the fallback to be reached at all, isFull() must be false, so
    // current < capacity; for it to fire, capacity must be <= 0. Together
    // that means current < 0. So a capacity-0 generator with zero or positive
    // charge never reached the invented 10000 at all — it was silently parked
    // by isFull(). The invented number only ever appeared for a generator
    // holding NEGATIVE energy, where it turned a corrupt -5 HU buffer into a
    // full 10000 HU one, published a 10000 bar to the client, and burned coal
    // to do it.
    //
    // Both halves are pinned: the fuel must survive, and the published capacity
    // must not be 10000.
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, /*capacity=*/0, kDeclaredMaxOutput,
                             kCoal, /*fuel_count=*/2, /*current=*/-5);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(energy(f->reg, ent).current, -5,
                 "a capacity-0 generator holding negative energy is not repaired to 10000");
    CHECK_EQ_INT(energy(f->reg, ent).capacity, 0,
                 "the invented 10000 is not written back into the component");
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 2,
                 "and the coal is not burned to fill a buffer that has no declared size");
    CHECK(f->events->updates.empty(),
          "no block-entity update advertises a 10000 bar for this machine");
}

// ---------------------------------------------------------------------------
// C. The same rule governs max_output
// ---------------------------------------------------------------------------

static void test_GeneratorSystem_zero_max_output_is_refused() {
    auto f = makeFixture();
    // capacity is healthy here, so the tick reaches the gate on the
    // max_output half of the condition (and is not short-circuited by isFull).
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, kDeclaredCapacity, /*max_output=*/0,
                             kCoal, /*fuel_count=*/2);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(energy(f->reg, ent).current, 0,
                 "a declared max_output of 0 means the machine can never emit");
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 2,
                 "the coal is NOT consumed when max_output is unusable");
}

static void test_GeneratorSystem_negative_capacity_is_refused() {
    // A negative value is corruption, not a "small" buffer. The check is <= 0 so
    // it is refused rather than turned into a negative produceEnergy() charge.
    // current 5 keeps the entity out of the isFull() short-circuit.
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, /*capacity=*/-100,
                             kDeclaredMaxOutput, kCoal, /*fuel_count=*/1, /*current=*/5);

    f->sys->tick(0.05f);
    CHECK_EQ_INT(energy(f->reg, ent).current, 5, "a negative capacity charges nothing");
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 1, "and consumes no fuel");
}

// ---------------------------------------------------------------------------
// D. The refusal does not go sticky
// ---------------------------------------------------------------------------

static void test_GeneratorSystem_recovering_capacity_starts_charging_next_tick() {
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, /*capacity=*/0, kDeclaredMaxOutput,
                             kCoal, /*fuel_count=*/2, /*current=*/5);

    f->sys->tick(0.05f);
    CHECK_EQ_INT(energy(f->reg, ent).current, 5, "misconfigured: nothing charges");

    // The operator fixes machines.yaml. SimulationEngine re-seeds the
    // component on the next block update; the system must not need a restart,
    // so there is deliberately no latch in burnEnergy_/burnFuel_.
    energy(f->reg, ent).capacity = kDeclaredCapacity;

    f->sys->tick(0.05f);
    // The 5 HU already in the buffer is kept, not discarded: the fix restores
    // a declared size, it does not reset the machine.
    CHECK_EQ_INT(energy(f->reg, ent).current, 5 + kDeclaredMaxOutput,
                 "once the data is fixed the very next tick charges normally");
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 1,
                 "and the recovered generator burns exactly one coal");
}

// ---------------------------------------------------------------------------
// E. The check does not fire on a healthy idle generator
// ---------------------------------------------------------------------------

static void test_GeneratorSystem_well_configured_empty_generator_is_quiet() {
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kCoalGeneratorId, kDeclaredCapacity, kDeclaredMaxOutput,
                             /*fuel_id=*/0, /*fuel_count=*/0);

    f->sys->tick(0.05f);
    f->sys->tick(0.05f);

    // A correctly configured generator that simply has no fuel is idle, not
    // misconfigured — the new gate must not turn "out of coal" into an error
    // path, and it must not publish a partial burn either.
    CHECK_EQ_INT(energy(f->reg, ent).current, 0, "an empty generator stays empty");
    CHECK(f->events->updates.empty(),
          "an empty but well-configured generator publishes nothing (no burn happened)");
}

static void test_GeneratorSystem_non_generator_is_skipped_before_the_capacity_check() {
    // A machine whose id is not a generator must be ignored even if its buffer
    // is zero — proves the capacity gate did not become the only entry filter.
    auto f = makeFixture();
    auto ent = makeGenerator(f->reg, kNotAGenerator, /*capacity=*/0, /*max_output=*/0,
                             kCoal, /*fuel_count=*/1);

    f->sys->tick(0.05f);
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 1,
                 "a non-generator keeps its inventory untouched");
    CHECK(f->events->updates.empty(), "a non-generator publishes nothing");
}

// ---------------------------------------------------------------------------
// F. The steam-id gate still wins
// ---------------------------------------------------------------------------

static void test_GeneratorSystem_steam_gate_precedes_the_capacity_check() {
    // steam_id 0 fails closed at GeneratorSystem.cpp:45, which is BEFORE the
    // new capacity check. So a STEAM generator with a healthy buffer and no
    // steam id is quiet, and its fuel is untouched.
    auto f = makeFixture(/*steam_item_id=*/0);
    auto ent = makeGenerator(f->reg, kSteamBoilerId, kDeclaredCapacity, kDeclaredMaxOutput,
                             kCoal, /*fuel_count=*/3, 0, EnergyType::STEAM);

    f->sys->tick(0.05f);
    f->sys->tick(0.05f);

    CHECK_EQ_INT(energy(f->reg, ent).current, 0,
                 "a steam generator with no steam id does not charge");
    CHECK_EQ_INT(container(f->reg, ent).slots[0].count, 3,
                 "and does not burn fuel (the steam gate comes first)");

    // With a resolved steam id the same entity charges normally. Note the
    // STEAM branch publishes the node every tick (line 49) BEFORE the burn, so
    // a well-configured steam generator does emit an update even with no fuel.
    auto f2 = makeFixture(/*steam_item_id=*/ItemId::pack("1111:11:1"));
    auto ent2 = makeGenerator(f2->reg, kSteamBoilerId, kDeclaredCapacity, kDeclaredMaxOutput,
                              kCoal, /*fuel_count=*/3, 0, EnergyType::STEAM);
    f2->sys->tick(0.05f);
    CHECK_EQ_INT(energy(f2->reg, ent2).current, kDeclaredMaxOutput,
                 "with a steam id the solid boiler charges the declared rate");
    CHECK_EQ_INT(container(f2->reg, ent2).slots[0].count, 2,
                 "and consumes exactly one coal for the burn");
}

// ---------------------------------------------------------------------------
// G. The published capacity is the real one
// ---------------------------------------------------------------------------

static void test_GeneratorSystem_publishes_the_real_capacity() {
    // The counterpart to gp-bbbl, where RotareGeneratorSystem publishes 0 and
    // the client falls back to tier*10000 (MachineWindow.cpp:426-428). Here the
    // value published is whatever machines.yaml declared — here a deliberately
    // non-10000 number, so a reintroduced constant would fail the test.
    auto f = makeFixture();
    makeGenerator(f->reg, kCoalGeneratorId, /*capacity=*/7777, kDeclaredMaxOutput);

    f->sys->tick(0.05f);

    const PublisherEvent* ev = lastUpdate(*f);
    CHECK(ev != nullptr, "a burning generator publishes a block-entity update");
    if (!ev) return;
    CHECK_EQ_INT(ev->energy_capacity, 7777u,
                 "the published capacity is the declared one, not 10000 and not 0");
    CHECK_EQ_INT(ev->energy, static_cast<uint32_t>(kDeclaredMaxOutput),
                 "the published energy is one tick of the declared rate");
    CHECK_EQ_INT(static_cast<int>(ev->energy_type), static_cast<int>(EnergyType::HEAT),
                 "the coal generator publishes its HEAT energy type");
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== generator_system test suite ===\n\n");

    // Declared constants
    TEST(GeneratorSystem_declared_constants);

    // The declared values are the burn parameters
    TEST(GeneratorSystem_declared_max_output_is_the_burn_rate);
    TEST(GeneratorSystem_production_is_clamped_by_the_declared_capacity);

    // The removed fallback
    TEST(GeneratorSystem_zero_capacity_burns_nothing_and_keeps_its_fuel);
    TEST(GeneratorSystem_zero_capacity_publishes_no_invented_bar);
    TEST(GeneratorSystem_zero_capacity_at_zero_charge_is_refused_not_treated_as_full);
    TEST(GeneratorSystem_zero_capacity_with_negative_charge_is_the_reachable_fallback_case);
    TEST(GeneratorSystem_zero_max_output_is_refused);
    TEST(GeneratorSystem_negative_capacity_is_refused);

    // Recovery
    TEST(GeneratorSystem_recovering_capacity_starts_charging_next_tick);

    // The gate does not over-fire
    TEST(GeneratorSystem_well_configured_empty_generator_is_quiet);
    TEST(GeneratorSystem_non_generator_is_skipped_before_the_capacity_check);

    // Gate ordering
    TEST(GeneratorSystem_steam_gate_precedes_the_capacity_check);

    // Published capacity (contrast gp-bbbl)
    TEST(GeneratorSystem_publishes_the_real_capacity);

    MachineRegistry::setInstance(nullptr);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
