// RotareGeneratorSystem unit tests (issue gp-kxj5).
//
// Covers src/game/machines/RotareGeneratorSystem.cpp — the click-to-spin
// kinetic generator that emits ROTATION energy for a fixed number of ticks.
//
// VIEW LIVENESS (checked before writing these tests):
//   reg_.view<MachineComponent, EnergyStorage, RotareState>()
// MachineComponent and EnergyStorage ARE emplaced by SimulationEngine
// (lines 219 / 259) for "1110:100:1" (rotare_generator), which machines.yaml
// registers. RotareState is NOT emplaced anywhere in production code: it is
// only ever created by RotareGeneratorSystem::activate(), and
// `grep -rn 'activate(' src/` shows the only two hits are the declaration in
// the header and the definition in the .cpp — nothing calls it. So even though
// machines.yaml flags this block `interact_on_left: true` and the action
// dispatcher routes left-clicks to an interact handler, no interact handler
// ever reaches activate().
//
// FINDING: RotareGeneratorSystem is DEAD AT RUNTIME, and unlike
// SteamTurbineSystem (whose SteamTurbineComponent is at least a dedicated
// component a caller would attach) there is no code path that can create the
// RotareState that satisfies the view. The spin the player pays fuel-adjacent
// world state for never starts. The tests below exercise the real logic so the
// gate is pinned if the wiring is ever added.
//
// SYSTEM CONTRACT:
//   activate(ent): if a RotareState exists and is already spinning, return.
//     Otherwise emplace_or_replace<RotareState>{spinning=true,
//     remainingTicks=kSpinDurationTicks, energyPerTick=kEnergyPerTick}.
//   tick(dt), for each entity in the view:
//     0. skip unless machine_id == kRotareGeneratorBlockId
//     1. skip unless state.spinning
//     2. if remainingTicks <= 0: spinning = false; continue
//     3. space = capacity - current; toAdd = min(energyPerTick, space)
//     4. if toAdd <= 0: spinning = false; continue   (stop on a full buffer)
//     5. current += toAdd; remainingTicks--
//     6. publish the node, then publishBlockEntityUpdate with
//        progress = remainingTicks / kSpinDurationTicks
//     7. if remainingTicks <= 0: spinning = false
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed"):
//   A. A FULL BUFFER COSTS THE WHOLE SPIN. The toAdd <= 0 branch sets
//      spinning = false, so filling the ROTATION buffer mid-spin aborts it
//      permanently rather than pausing. Pinned by
//      test_RotareGeneratorSystem_full_buffer_aborts_the_spin.
//   B. THE PARTIAL FILL TICK IS PAID FOR. A buffer with 10 EU of space and
//      energyPerTick 32 adds 10 and still decrements remainingTicks, so the
//      tick is spent without the full rate. Pinned by
//      test_RotareGeneratorSystem_partial_fill_still_burns_a_tick.
//   C. THE PROGRESS VALUE IS A SPIN REMAINING FRACTION, NOT COMPLETION.
//      remainingTicks / kSpinDurationTicks runs 0.99 -> 0.0 and is published on
//      the same field the other machine systems use for craft progress, so the
//      client's progress bar runs backwards. Note the value is computed AFTER
//      the decrement (RotareGeneratorSystem.cpp:36,59), so the series is
//      99/100, 98/100, ... 0/100 — it ends at exactly 0.0 and never at 1.0.
//      Pinned by test_RotareGeneratorSystem_progress_runs_from_full_to_empty.
//   D. THE ROTATION CAPACITY IS NEVER PUBLISHED. The publishBlockEntityUpdate
//      call stops at energy.type (RotareGeneratorSystem.cpp:55-61), so the
//      `energy_capacity` argument falls through to the IEventPublisher.h:52
//      default of 0. Every sibling system passes energy.capacity. Pinned by
//      test_RotareGeneratorSystem_publishes_one_update_per_spinning_tick.
//   E. THE MACHINE-ID GATE SKIPS BEFORE THE STOP LOGIC. A foreign machine_id
//      hits `continue` at RotareGeneratorSystem.cpp:21, so its RotareState is
//      never read or stopped. Harmless today (only a rotare_generator can
//      carry the state) but it means a stale RotareState on a re-purposed
//      entity spins forever. Pinned by
//      test_RotareGeneratorSystem_block_id_matches_the_rotation_machine.
//
// FILED AS BEADS ISSUES (production bugs, NOT fixed here — no src/ edits):
//   gp-18yv  nothing calls RotareGeneratorSystem::activate(), so in-game the
//            rotare_generator can never spin at all. The tests below call
//            activate() directly, so they pin the logic while the wiring is
//            still missing.
//   gp-bbbl  the publishBlockEntityUpdate call omits energy_capacity, so
//            clients fall back to a tier guess and render a 10000 EU bar for a
//            buffer machines.yaml declares as 5000. Pinned as-is below; flip
//            that one assertion to == kCapacity when the issue is fixed.
#include <cstdio>
#include <cstdint>
#include <array>
#include <memory>
#include <vector>

#include <entt/entt.hpp>

#include <engine/registry/ItemId.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/MachineComponent.h>
#include <game/machines/RotareGeneratorSystem.h>

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

// Null pipe client: RotareGeneratorSystem guards every publishNodeUpdate with
// `if (pipeClient_)`, so the conversion path runs with no router or socket.
struct Fixture {
    entt::registry reg;
    std::shared_ptr<RecordingPublisher> events =
        std::make_shared<RecordingPublisher>();
    simcore::RotareGeneratorSystem sys{reg, events, nullptr};
};

// machines.yaml rotare_generator: capacity 5000, max_output 64, tier 0.
static constexpr uint16_t kRotareId = ItemId::pack("1110:100:1");
static constexpr int32_t kCapacity = 5000;
static constexpr int32_t kMaxOutput = 64;
static constexpr int32_t kTier = 0;

// A rotare_generator with an EnergyStorage matching machines.yaml. RotareState
// is attached separately so each test controls the spin.
static entt::entity makeRotare(entt::registry& reg, uint32_t x, uint32_t y, uint32_t z,
                               int32_t current, int32_t capacity = kCapacity) {
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, kRotareId, 0, x, y, z, 1);
    reg.emplace<simcore::EnergyStorage>(ent, capacity, current, kMaxOutput, kMaxOutput,
                                        kTier, simcore::EnergyType::ROTATION);
    return ent;
}

static simcore::RotareState& state(entt::registry& reg, entt::entity ent) {
    return reg.get<simcore::RotareState>(ent);
}
static simcore::EnergyStorage& energy(entt::registry& reg, entt::entity ent) {
    return reg.get<simcore::EnergyStorage>(ent);
}

// ---------------------------------------------------------------------------
// Constants, asserted by name
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_declared_constants() {
    CHECK_EQ_INT(simcore::RotareGeneratorSystem::kRotareGeneratorBlockId, kRotareId,
                 "the system targets the rotare_generator block in machines.yaml");
    CHECK_EQ_INT(simcore::RotareGeneratorSystem::kSpinDurationTicks, 100,
                 "a spin lasts 100 ticks (5 s at the 20 Hz sim rate)");
    CHECK_EQ_INT(simcore::RotareGeneratorSystem::kEnergyPerTick, 32,
                 "a spin emits 32 ROTATION per tick");
    // 100 ticks * 32 = 3200 ROTATION per full spin, inside the 5000 capacity
    // declared in machines.yaml.
    CHECK_EQ_INT(simcore::RotareGeneratorSystem::kSpinDurationTicks *
                     simcore::RotareGeneratorSystem::kEnergyPerTick,
                 3200, "a full spin yields 3200 ROTATION");
    CHECK(simcore::RotareGeneratorSystem::kEnergyPerTick *
              simcore::RotareGeneratorSystem::kSpinDurationTicks < kCapacity,
          "a full spin fits inside the declared ROTATION buffer");
}

static void test_RotareGeneratorSystem_default_state_is_not_spinning() {
    simcore::RotareState fresh;
    CHECK(!fresh.spinning, "a default-constructed RotareState is not spinning");
    CHECK_EQ_INT(fresh.remainingTicks, 0, "and has no remaining ticks");
    CHECK_EQ_INT(fresh.energyPerTick, 32, "the default per-tick rate matches kEnergyPerTick");
}

// ---------------------------------------------------------------------------
// activate()
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_activate_creates_a_full_spin() {
    Fixture f;
    auto ent = makeRotare(f.reg, 10, 64, 10, /*current=*/0);
    f.sys.activate(ent);
    const auto& s = state(f.reg, ent);
    CHECK(s.spinning, "activate() starts the spin");
    CHECK_EQ_INT(s.remainingTicks, simcore::RotareGeneratorSystem::kSpinDurationTicks,
                 "a fresh spin lasts kSpinDurationTicks");
    CHECK_EQ_INT(s.energyPerTick, simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "a fresh spin runs at kEnergyPerTick");
}

static void test_RotareGeneratorSystem_activate_is_a_noop_while_spinning() {
    Fixture f;
    auto ent = makeRotare(f.reg, 11, 64, 11, 0);
    f.sys.activate(ent);
    // Burn 40 ticks of the spin, then click again.
    for (int i = 0; i < 40; ++i) f.sys.tick(0.05f);
    const int32_t remaining = state(f.reg, ent).remainingTicks;
    CHECK(remaining < simcore::RotareGeneratorSystem::kSpinDurationTicks,
          "precondition: the spin is partly spent");

    f.sys.activate(ent);
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks, remaining,
                 "clicking again mid-spin does not refill the spin");
}

static void test_RotareGeneratorSystem_activate_restarts_a_finished_spin() {
    Fixture f;
    auto ent = makeRotare(f.reg, 12, 64, 12, 0);
    f.sys.activate(ent);
    // Run the spin out (100 ticks at 32 = 3200 into a 5000 buffer).
    for (int i = 0; i < simcore::RotareGeneratorSystem::kSpinDurationTicks; ++i) {
        f.sys.tick(0.05f);
    }
    CHECK(!state(f.reg, ent).spinning, "the spin stops after kSpinDurationTicks");

    f.sys.activate(ent);
    CHECK(state(f.reg, ent).spinning, "clicking after the spin restarts it");
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks,
                 simcore::RotareGeneratorSystem::kSpinDurationTicks,
                 "the restarted spin is a full one");
}

static void test_RotareGeneratorSystem_activate_resets_a_halted_spin() {
    Fixture f;
    // A tiny buffer: the first tick fills it, and the toAdd <= 0 branch halts
    // the spin. A fresh click must give the player the full spin back.
    auto ent = makeRotare(f.reg, 13, 64, 13, 0, /*capacity=*/10);
    f.sys.activate(ent);
    f.sys.tick(0.05f);
    f.sys.tick(0.05f);
    CHECK(!state(f.reg, ent).spinning, "a full buffer halts the spin");

    f.sys.activate(ent);
    CHECK(state(f.reg, ent).spinning, "clicking again restarts the halted spin");
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks,
                 simcore::RotareGeneratorSystem::kSpinDurationTicks,
                 "the restart is a full kSpinDurationTicks");
}

static void test_RotareGeneratorSystem_activate_replaces_a_stale_state() {
    Fixture f;
    auto ent = makeRotare(f.reg, 14, 64, 14, 0);
    // A RotareState left over from an aborted spin, not spinning.
    f.reg.emplace<simcore::RotareState>(ent,
                                        simcore::RotareState{false, 7, 999});
    f.sys.activate(ent);
    const auto& s = state(f.reg, ent);
    CHECK(s.spinning, "activate() takes over a non-spinning stale state");
    CHECK_EQ_INT(s.remainingTicks, simcore::RotareGeneratorSystem::kSpinDurationTicks,
                 "the stale remainingTicks is overwritten");
    CHECK_EQ_INT(s.energyPerTick, simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "a stale custom rate is overwritten with the declared one");
}

static void test_RotareGeneratorSystem_activate_on_a_stale_entity_is_safe() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    simcore::RotareGeneratorSystem sys(reg, events, nullptr);
    // No MachineComponent/EnergyStorage: activate() only touches RotareState.
    auto ent = reg.create();
    sys.activate(ent);
    CHECK(reg.all_of<simcore::RotareState>(ent),
          "activate() creates a RotareState even without the rest of the view");
}

// ---------------------------------------------------------------------------
// The generation rate
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_produces_energy_per_tick_at_the_declared_rate() {
    Fixture f;
    auto ent = makeRotare(f.reg, 20, 64, 20, 0);
    f.sys.activate(ent);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "one tick emits exactly kEnergyPerTick ROTATION");
}

static void test_RotareGeneratorSystem_accumulates_over_the_whole_spin() {
    Fixture f;
    auto ent = makeRotare(f.reg, 21, 64, 21, 0);
    f.sys.activate(ent);
    for (int i = 1; i <= 10; ++i) {
        f.sys.tick(0.05f);
        CHECK_EQ_INT(energy(f.reg, ent).current,
                     i * simcore::RotareGeneratorSystem::kEnergyPerTick,
                     "the buffer grows by exactly kEnergyPerTick per tick");
    }
}

static void test_RotareGeneratorSystem_a_full_spin_yields_the_declared_total() {
    Fixture f;
    auto ent = makeRotare(f.reg, 22, 64, 22, 0);
    f.sys.activate(ent);
    for (int i = 0; i < simcore::RotareGeneratorSystem::kSpinDurationTicks; ++i) {
        f.sys.tick(0.05f);
    }
    CHECK_EQ_INT(energy(f.reg, ent).current,
                 simcore::RotareGeneratorSystem::kSpinDurationTicks *
                     simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "a full spin yields kSpinDurationTicks * kEnergyPerTick ROTATION");
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks, 0, "the spin is exhausted");
    CHECK(!state(f.reg, ent).spinning, "and the generator has stopped");
}

static void test_RotareGeneratorSystem_generation_adds_to_an_existing_charge() {
    Fixture f;
    auto ent = makeRotare(f.reg, 23, 64, 23, /*current=*/1000);
    f.sys.activate(ent);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 1000 + simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "the spin tops up whatever is already buffered");
}

static void test_RotareGeneratorSystem_production_ignores_max_output() {
    // machines.yaml declares max_output 64, above kEnergyPerTick 32, so the
    // cap never binds. Documented so a future max_output drop is visible: the
    // system calls energy.current += directly, so a max_output below 32 would
    // NOT throttle the spin.
    Fixture f;
    auto ent = makeRotare(f.reg, 24, 64, 24, 0);
    // A max_output of 1, far below the 32/tick rate.
    energy(f.reg, ent).maxOutput = 1;
    f.sys.activate(ent);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "maxOutput does not throttle generator production");
    CHECK_EQ_INT(energy(f.reg, ent).maxOutput, 1, "precondition: maxOutput really was 1");
}

// ---------------------------------------------------------------------------
// The stop conditions
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_not_spinning_produces_nothing() {
    Fixture f;
    auto ent = makeRotare(f.reg, 30, 64, 30, 0);
    // No activate(): the entity carries no RotareState and is outside the view.
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "an un-spun generator produces nothing");
    CHECK_EQ_INT(f.events->updates.size(), size_t(0), "and publishes nothing");
}

static void test_RotareGeneratorSystem_a_stopped_state_stays_stopped() {
    Fixture f;
    auto ent = makeRotare(f.reg, 31, 64, 31, 0);
    f.reg.emplace<simcore::RotareState>(ent, simcore::RotareState{false, 50, 32});
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "spinning=false produces nothing");
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks, 50,
                 "and the remainingTicks of a stopped state are not advanced");
}

static void test_RotareGeneratorSystem_an_expired_spin_stops_without_producing() {
    Fixture f;
    auto ent = makeRotare(f.reg, 32, 64, 32, 0);
    // remainingTicks 0 with spinning still true: the expiry branch runs, stops
    // the spin, and produces nothing.
    f.reg.emplace<simcore::RotareState>(ent, simcore::RotareState{true, 0, 32});
    f.sys.tick(0.05f);
    CHECK(!state(f.reg, ent).spinning, "an expired spin is stopped");
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "an expired spin produces nothing on its last tick");
    CHECK_EQ_INT(f.events->updates.size(), size_t(0), "and publishes nothing");
}

static void test_RotareGeneratorSystem_negative_remaining_ticks_also_stop() {
    Fixture f;
    auto ent = makeRotare(f.reg, 33, 64, 33, 0);
    f.reg.emplace<simcore::RotareState>(ent, simcore::RotareState{true, -5, 32});
    f.sys.tick(0.05f);
    CHECK(!state(f.reg, ent).spinning, "a negative remainingTicks also stops the spin");
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "and produces nothing");
}

static void test_RotareGeneratorSystem_stops_after_exactly_kSpinDuration_ticks() {
    Fixture f;
    auto ent = makeRotare(f.reg, 34, 64, 34, 0);
    f.sys.activate(ent);
    // Ticks 1..99 keep it spinning; tick 100 exhausts and stops it.
    for (int i = 1; i < simcore::RotareGeneratorSystem::kSpinDurationTicks; ++i) {
        f.sys.tick(0.05f);
        CHECK(state(f.reg, ent).spinning, "the spin is still running before the last tick");
    }
    f.sys.tick(0.05f);
    CHECK(!state(f.reg, ent).spinning, "the spin stops on the kSpinDurationTicks'th tick");
    CHECK_EQ_INT(energy(f.reg, ent).current,
                 simcore::RotareGeneratorSystem::kSpinDurationTicks *
                     simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "and that last tick still produced its full rate");
}

// ---------------------------------------------------------------------------
// FINDING A: a full buffer costs the whole spin
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_full_buffer_aborts_the_spin() {
    Fixture f;
    auto ent = makeRotare(f.reg, 40, 64, 40, 0, /*capacity=*/100);
    f.sys.activate(ent);
    // 100 / 32 = 3 full ticks plus a 4th that adds the remaining 4:
    // 32, 64, 96, 100. The 5th tick finds no space and aborts.
    for (int i = 0; i < 4; ++i) {
        f.sys.tick(0.05f);
        CHECK(state(f.reg, ent).spinning, "the spin survives while there is still headroom");
    }
    CHECK_EQ_INT(energy(f.reg, ent).current, 100, "the buffer is exactly full");
    f.sys.tick(0.05f);
    CHECK(!state(f.reg, ent).spinning,
          "the toAdd <= 0 branch aborts the spin rather than pausing it");
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks,
                 simcore::RotareGeneratorSystem::kSpinDurationTicks - 4,
                 "the aborted spin keeps its 96 unspent ticks but never spends them");
}

static void test_RotareGeneratorSystem_a_full_buffer_costs_the_remaining_spin() {
    Fixture f;
    auto ent = makeRotare(f.reg, 41, 64, 41, 0, /*capacity=*/32);
    f.sys.activate(ent);
    // Tick 1 exactly fills the 32 buffer, so the spin is still running.
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 32, "tick 1 exactly fills the 32 buffer");
    CHECK(state(f.reg, ent).spinning, "and the spin is still running");
    // Tick 2 finds no space and aborts.
    f.sys.tick(0.05f);
    CHECK(!state(f.reg, ent).spinning, "tick 2 aborts the spin on the full buffer");
    // No further ticks can change anything without a fresh click.
    for (int i = 0; i < 50; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 32, "and the 98 unspent ticks are never recovered");
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks,
                 simcore::RotareGeneratorSystem::kSpinDurationTicks - 1,
                 "the unspent spin is stranded, not resumed");
}

// ---------------------------------------------------------------------------
// FINDING B: a partial fill is paid for as a whole tick
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_partial_fill_still_burns_a_tick() {
    Fixture f;
    // 20 EU of space against a 32/tick rate: the tick adds the 20 available,
    // fills the buffer, AND decrements remainingTicks — the player pays for a
    // tick that only yielded 20 of the promised 32.
    auto ent = makeRotare(f.reg, 42, 64, 42, 0, /*capacity=*/20);
    f.sys.activate(ent);
    f.sys.tick(0.05f);
    const auto& s = state(f.reg, ent);
    CHECK_EQ_INT(energy(f.reg, ent).current, 20, "only the available 20 is added");
    CHECK_EQ_INT(s.remainingTicks, simcore::RotareGeneratorSystem::kSpinDurationTicks - 1,
                 "but the tick is still charged against the spin");
}

static void test_RotareGeneratorSystem_zero_space_stops_without_burning_a_tick() {
    Fixture f;
    // Starting exactly full: toAdd is 0, so the stop branch runs BEFORE the
    // decrement and the tick is not charged.
    auto ent = makeRotare(f.reg, 43, 64, 43, /*current=*/100, /*capacity=*/100);
    f.sys.activate(ent);
    f.sys.tick(0.05f);
    const auto& s = state(f.reg, ent);
    CHECK(!s.spinning, "a generator that starts full stops at once");
    CHECK_EQ_INT(s.remainingTicks, simcore::RotareGeneratorSystem::kSpinDurationTicks,
                 "the aborted tick is not charged against the spin");
    CHECK_EQ_INT(energy(f.reg, ent).current, 100, "and no energy is produced");
}

// ---------------------------------------------------------------------------
// FINDING C: the published progress is a spin-remaining fraction
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_progress_runs_from_full_to_empty() {
    Fixture f;
    auto ent = makeRotare(f.reg, 50, 64, 50, 0);
    f.sys.activate(ent);

    // The value published on the shared `progress` field is
    // remainingTicks / kSpinDurationTicks, so it starts at 1.0 and falls to
    // ~0 — the opposite sense to the craft progress the client renders.
    f.sys.tick(0.05f);
    if (!f.events->updates.empty()) {
        const float expected =
            static_cast<float>(simcore::RotareGeneratorSystem::kSpinDurationTicks - 1) /
            static_cast<float>(simcore::RotareGeneratorSystem::kSpinDurationTicks);
        CHECK_EQ_INT(f.events->updates[0].progress, expected,
                     "the first published progress is (remaining)/kSpinDurationTicks");
    }

    for (int i = 0; i < simcore::RotareGeneratorSystem::kSpinDurationTicks - 1; ++i) {
        f.sys.tick(0.05f);
    }
    if (f.events->updates.size() == simcore::RotareGeneratorSystem::kSpinDurationTicks) {
        const float last = f.events->updates.back().progress;
        const float first = f.events->updates.front().progress;
        // The value published is remainingTicks / kSpinDurationTicks AFTER the
        // decrement (RotareGeneratorSystem.cpp:59), so the series is exactly
        // 99/100, 98/100, ... 0/100: the last publish is exactly 0.0, not a
        // small epsilon. The tick that would report 1.0 (progress == start) is
        // the one the system never publishes, because the first publish already
        // carries a decremented remainingTicks.
        CHECK_EQ_INT(first,
                     static_cast<float>(simcore::RotareGeneratorSystem::kSpinDurationTicks - 1) /
                         static_cast<float>(simcore::RotareGeneratorSystem::kSpinDurationTicks),
                     "the first published progress is 99/100, one tick already spent");
        CHECK_EQ_INT(last, 0.0f,
                     "the last published progress is exactly zero, not a small epsilon");
        CHECK(last < first, "and the series runs downwards, 0.99 -> 0.0");
    }
}

static void test_RotareGeneratorSystem_publishes_one_update_per_spinning_tick() {
    Fixture f;
    auto ent = makeRotare(f.reg, 51, 64, 51, 0);
    f.sys.activate(ent);
    for (int i = 0; i < 5; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(f.events->updates.size(), size_t(5),
                 "the generator publishes on every spinning tick");
    if (f.events->updates.size() == 5) {
        const auto& u = f.events->updates[0];
        CHECK_EQ_INT(u.x, 51, "publishes the machine x");
        CHECK_EQ_INT(u.y, 64, "publishes the machine y");
        CHECK_EQ_INT(u.z, 51, "publishes the machine z");
        CHECK_EQ_INT(u.machine_id, kRotareId, "publishes the rotare_generator machine id");
        CHECK_EQ_INT(u.energy, simcore::RotareGeneratorSystem::kEnergyPerTick,
                     "publishes the post-tick buffer level");
        // FINDING D: the call site (RotareGeneratorSystem.cpp:55-61) stops at
        // energy.type and never passes the `energy_capacity` argument, so it
        // falls through to the IEventPublisher.h:52 default of 0. Every other
        // machine system passes energy.capacity here (GeneratorSystem.cpp:206,
        // EBFSystem.cpp:331, LCRSystem.cpp:252, MachineSystem.cpp:109,555).
        // MachineWindow.cpp:426-428 only trusts the wire value when it is > 0
        // and otherwise falls back to a tier-derived guess, so a tier-0
        // rotare_generator is drawn with a 10000 EU bar instead of its
        // declared 5000. Pinned as-is: beads gp-bbbl.
        CHECK_EQ_INT(u.energy_capacity, 0u,
                     "the capacity is NOT published — the call relies on the "
                     "IEventPublisher default, unlike every sibling system");
        CHECK_EQ_INT(int(u.energy_type), int(simcore::EnergyType::ROTATION),
                     "publishes the ROTATION energy type, not ELECTRICITY");
    }
}

// ---------------------------------------------------------------------------
// View membership and gating
// ---------------------------------------------------------------------------

static void test_RotareGeneratorSystem_block_id_matches_the_rotation_machine() {
    // machines.yaml registers 1110:100:1 with energy_out: ROTATION. No other
    // machine shares the id, so a wrong-id entity is always ignored.
    Fixture f;
    auto ent = makeRotare(f.reg, 60, 64, 60, 0);
    f.reg.get<simcore::MachineComponent>(ent).machine_id = ItemId::pack("1110:100:0");
    f.sys.activate(ent);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "a different machine id is ignored");
    // The id gate is `if (machine_id != kRotareGeneratorBlockId) continue;`
    // (RotareGeneratorSystem.cpp:21), which runs BEFORE any of the stop logic.
    // So a foreign machine is not merely left alone — its RotareState is
    // never inspected, let alone stopped. The spin flag stays true.
    CHECK(state(f.reg, ent).spinning,
          "a foreign machine id is skipped by `continue` before the stop "
          "branches, so its state is never touched");
    CHECK_EQ_INT(state(f.reg, ent).remainingTicks,
                 simcore::RotareGeneratorSystem::kSpinDurationTicks,
                 "and its remainingTicks are never advanced");
    CHECK_EQ_INT(f.events->updates.size(), size_t(0),
                 "a foreign machine id publishes nothing");
}

static void test_RotareGeneratorSystem_missing_rotare_state_is_outside_the_view() {
    Fixture f;
    auto ent = makeRotare(f.reg, 61, 64, 61, 0);
    // MachineComponent + EnergyStorage only: the state this is the state of
    // every real rotare_generator, since nothing in production calls activate().
    for (int i = 0; i < 5; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0,
                 "a rotare_generator without RotareState is outside the view");
    CHECK_EQ_INT(f.events->updates.size(), size_t(0), "and publishes nothing");
}

static void test_RotareGeneratorSystem_missing_energy_storage_is_outside_the_view() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    simcore::RotareGeneratorSystem sys(reg, events, nullptr);
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, kRotareId, 0, 1, 2, 3, 1);
    reg.emplace<simcore::RotareState>(ent, simcore::RotareState{true, 100, 32});
    for (int i = 0; i < 5; ++i) sys.tick(0.05f);
    CHECK_EQ_INT(events->updates.size(), size_t(0),
                 "without EnergyStorage the entity is outside the view");
    CHECK_EQ_INT(reg.get<simcore::RotareState>(ent).remainingTicks, 100,
                 "and its spin is not advanced");
}

static void test_RotareGeneratorSystem_empty_registry_is_a_noop() {
    Fixture f;
    f.sys.tick(0.05f);
    f.sys.tick(1.0f);
    CHECK_EQ_INT(f.events->updates.size(), size_t(0), "no entities -> no events");
}

static void test_RotareGeneratorSystem_multiple_generators_spin_independently() {
    Fixture f;
    auto a = makeRotare(f.reg, 70, 64, 70, 0);
    auto b = makeRotare(f.reg, 71, 64, 71, 0);
    auto c = makeRotare(f.reg, 72, 64, 72, 0, /*capacity=*/10);
    f.sys.activate(a);
    f.sys.activate(b);
    f.sys.activate(c);

    f.sys.tick(0.05f);
    f.sys.tick(0.05f);

    CHECK_EQ_INT(energy(f.reg, a).current, 2 * simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "the first generator spun for 2 ticks");
    CHECK_EQ_INT(energy(f.reg, b).current, 2 * simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "the second generator spun for 2 ticks");
    // c's buffer took 10 on tick 1 (spinning -> false), so tick 2 is inert.
    CHECK_EQ_INT(energy(f.reg, c).current, 10, "the third generator filled its 10 EU buffer");
    CHECK(!state(f.reg, c).spinning, "and its spin was aborted by the full buffer");
    CHECK(state(f.reg, a).spinning, "the first generator is unaffected by the third");
    // Tick-per-publish accounting, by position so the assertion does not
    // depend on entt view iteration order:
    //   tick 1 -> a, b and c all produce (c adds 10 and publishes), 3 events
    //   tick 2 -> c finds no space, sets spinning=false and `continue`s
    //              BEFORE publishing, so only a and b publish, 2 events
    int pub_a = 0, pub_b = 0, pub_c = 0;
    for (const auto& u : f.events->updates) {
        if (u.x == 70) ++pub_a;
        else if (u.x == 71) ++pub_b;
        else if (u.x == 72) ++pub_c;
    }
    CHECK_EQ_INT(f.events->updates.size(), size_t(5),
                 "three publishes on tick 1 plus only the two live ones on tick 2");
    CHECK_EQ_INT(pub_c, 1, "the third generator published on the tick that filled its buffer");
    CHECK_EQ_INT(pub_b, 2, "the second generator published on both ticks");
    CHECK_EQ_INT(pub_a, 2, "the first generator published on both ticks");
    // Keyed on x, not on index: entt iterates the view in reverse creation
    // order, so updates.front() is generator c (energy 10), not a or b.
    uint32_t first_pub_eu = 0;
    for (const auto& u : f.events->updates) {
        if (u.x == 70) { first_pub_eu = u.energy; break; }
    }
    CHECK_EQ_INT(first_pub_eu, simcore::RotareGeneratorSystem::kEnergyPerTick,
                 "the first tick publishes the post-tick level, not the pre-tick one");
}

static void test_RotareGeneratorSystem_dt_is_ignored() {
    // The spin is tick-count based, not time based: dt must not matter.
    for (float dt : {0.0f, 0.05f, 1.0f, 100.0f}) {
        Fixture f;
        auto ent = makeRotare(f.reg, 80, 64, 80, 0);
        f.sys.activate(ent);
        f.sys.tick(dt);
        CHECK_EQ_INT(energy(f.reg, ent).current,
                     simcore::RotareGeneratorSystem::kEnergyPerTick,
                     "production per tick is independent of dt");
        CHECK_EQ_INT(state(f.reg, ent).remainingTicks,
                     simcore::RotareGeneratorSystem::kSpinDurationTicks - 1,
                     "the spin count is independent of dt");
    }
}

static void test_RotareGeneratorSystem_stopped_generator_idles_silently() {
    Fixture f;
    auto ent = makeRotare(f.reg, 81, 64, 81, 0);
    f.sys.activate(ent);
    for (int i = 0; i < simcore::RotareGeneratorSystem::kSpinDurationTicks; ++i) {
        f.sys.tick(0.05f);
    }
    const int32_t published = static_cast<int32_t>(f.events->updates.size());
    for (int i = 0; i < 200; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(static_cast<int32_t>(f.events->updates.size()), published,
                 "a spent generator publishes nothing further");
    CHECK_EQ_INT(energy(f.reg, ent).current, 3200, "and produces nothing further");
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== rotare_generator_system test suite ===\n\n");

    TEST(RotareGeneratorSystem_declared_constants);
    TEST(RotareGeneratorSystem_default_state_is_not_spinning);

    TEST(RotareGeneratorSystem_activate_creates_a_full_spin);
    TEST(RotareGeneratorSystem_activate_is_a_noop_while_spinning);
    TEST(RotareGeneratorSystem_activate_restarts_a_finished_spin);
    TEST(RotareGeneratorSystem_activate_resets_a_halted_spin);
    TEST(RotareGeneratorSystem_activate_replaces_a_stale_state);
    TEST(RotareGeneratorSystem_activate_on_a_stale_entity_is_safe);

    TEST(RotareGeneratorSystem_produces_energy_per_tick_at_the_declared_rate);
    TEST(RotareGeneratorSystem_accumulates_over_the_whole_spin);
    TEST(RotareGeneratorSystem_a_full_spin_yields_the_declared_total);
    TEST(RotareGeneratorSystem_generation_adds_to_an_existing_charge);
    TEST(RotareGeneratorSystem_production_ignores_max_output);

    TEST(RotareGeneratorSystem_not_spinning_produces_nothing);
    TEST(RotareGeneratorSystem_a_stopped_state_stays_stopped);
    TEST(RotareGeneratorSystem_an_expired_spin_stops_without_producing);
    TEST(RotareGeneratorSystem_negative_remaining_ticks_also_stop);
    TEST(RotareGeneratorSystem_stops_after_exactly_kSpinDuration_ticks);

    TEST(RotareGeneratorSystem_full_buffer_aborts_the_spin);
    TEST(RotareGeneratorSystem_a_full_buffer_costs_the_remaining_spin);
    TEST(RotareGeneratorSystem_partial_fill_still_burns_a_tick);
    TEST(RotareGeneratorSystem_zero_space_stops_without_burning_a_tick);

    TEST(RotareGeneratorSystem_progress_runs_from_full_to_empty);
    TEST(RotareGeneratorSystem_publishes_one_update_per_spinning_tick);

    TEST(RotareGeneratorSystem_block_id_matches_the_rotation_machine);
    TEST(RotareGeneratorSystem_missing_rotare_state_is_outside_the_view);
    TEST(RotareGeneratorSystem_missing_energy_storage_is_outside_the_view);
    TEST(RotareGeneratorSystem_empty_registry_is_a_noop);
    TEST(RotareGeneratorSystem_multiple_generators_spin_independently);
    TEST(RotareGeneratorSystem_dt_is_ignored);
    TEST(RotareGeneratorSystem_stopped_generator_idles_silently);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
