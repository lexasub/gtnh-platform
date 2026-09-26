// BoilerSystem + LargeBoilerSystem unit tests (issue gp-g850).
//
// Covers src/game/machines/BoilerSystem.cpp (HEAT -> STEAM conversion) and
// src/game/machines/LargeBoilerSystem.cpp (solid fuel -> HEAT + steam
// saturation), both registered unconditionally in main.cpp:105 and :494.
//
// The two systems are unrelated code paths that share a name, so they get
// separate fixtures and separate test groups below.
//
// BOILER (heat boiler, 1110:011:1) — conversion invariants
//   CONVERSION_RATE            = 1    (HeatConstants.h:13)
//   HEAT_SINK_REPLENISH_TARGET = 100  (HeatConstants.h:18)
//   The conversion is `min(rate, heat_stored, steam_capacity - steam_stored)`
//   and the SAME amount is subtracted from heat_stored, energy.current and
//   added to steam_stored (BoilerSystem.cpp:53-60). So HEAT in == STEAM out,
//   one-for-one, with no efficiency loss. Pinned by
//   test_boiler_conversion_is_one_to_one.
//
//   The heatIntake.heat_stored = max(heat_stored, energy.current) sync at
//   BoilerSystem.cpp:46-48 is a MAX, not an assignment: a stale
//   HeatIntakeComponent can never lower the field, and a hot EnergyStorage can
//   only ever raise it. Pinned by test_boiler_heat_mirror_is_a_max_not_a_set.
//
// LARGE BOILER (solid fuel) — the interesting part
//   Fuel is coal (1010) or charcoal (1011); it burns at BOILER_HEAT_PER_FUEL
//   = 100 HU per unit and produces STEAM_PER_WATER = 10 steam per unit
//   (LargeBoilerSystem.h:31-32). The steam is added with
//   `energy->addEnergy(STEAM_PER_WATER)` (LargeBoilerSystem.cpp:81).
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed" here):
//
//   A. addEnergy() IS THE WRONG HELPER, AND maxInput TRUNCATES THE YIELD.
//      EnergyStorage::addEnergy clamps to maxInput (EnergyStorage.h:56-58),
//      which is documented as gating how much EXTERNAL energy a machine may
//      RECEIVE per tick. The LargeBoiler is producing, not receiving, so the
//      clamp is wrong in principle — and produceEnergy() exists precisely for
//      this case ("Clamps only to capacity — production is not limited by
//      maxInput", EnergyStorage.h:64-66) yet is NOT used.
//      Concretely: a boiler with the default maxInput of 0 produces 0 steam per
//      fuel and silently discards the rest; with maxInput == 1 it produces 1
//      instead of the documented 10.
//      Pinned by test_large_boiler_steam_yield_is_truncated_by_max_input and
//      test_large_boiler_zero_max_input_produces_no_steam.
//
//   B. THE HEAT GAIN IS NOT SATURATED AGAINST ENERGY, ONLY AGAINST THE
//      COMPONENT. `heatIntake.heat_stored += 100` is clamped to
//      heat_capacity (LargeBoilerSystem.cpp:75-77) but the EnergyStorage
//      mirror is never updated, so the two drift apart permanently. Pinned by
//      test_large_boiler_heat_is_capped_but_energy_is_not_mirrored.
//
//   C. OVERHEAT WARNING IS ONLY SET ON THE COLD PATH. The WARNING flag is
//      emplaced in the `else` (no fuel) branch, so a boiler burning fuel never
//      warns even at 100% heat — and a WARNING raised on a cold tick is not
//      cleared until fuel is found again. Pinned by
//      test_large_boiler_overheat_warning_requires_the_no_fuel_path.
//
//   D. THE COOLING RATE IS A HARD 5 HU/TICK, NOT dt-SCALED, and the tick
//      signature is `tick(float)` with the parameter unnamed and unused.
//      So the boiler cools 5 HU per TICK regardless of dt. Pinned by
//      test_large_boiler_cooling_is_five_per_tick_not_dt_scaled.
//
//   E. BOTH BOILERS PUBLISH energy_capacity = 0, so MachineWindow's
//      tier*10000 fallback (MachineWindow.cpp:426-428) fires and a client sees
//      a 10000-unit bar regardless of the real capacity. This is the same
//      client-visible class of bug as gp-bbbl, in the OPPOSITE direction: here
//      the system publishes a real capacity that is ignored... no — it
//      publishes LITERAL ZERO, so the fallback wins. Pinned by
//      test_boiler_publishes_zero_capacity.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include "Network/IEventPublisher.h"
#include <content/content.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/PatternLibrary.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/HeatIntakeComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/MultiblockController.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/RecipeProgress.h>
#include <engine/sim/components/SteamOutputComponent.h>
#include <game/machines/BoilerSystem.h>
#include <game/machines/LargeBoilerSystem.h>
#include <game/machines/HeatConstants.h>
#include <game/machines/OverheatComponent.h>

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has
// no GTest dependency, so this is the established convention for focused tests.
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
// Publisher double
// ---------------------------------------------------------------------------

struct PublisherEvent {
    int32_t x = 0, y = 0, z = 0;
    uint16_t machine_id = 0;
    float progress = 0.0f;
    uint32_t energy = 0;
    EnergyType energy_type = EnergyType::ELECTRICITY;
    uint32_t energy_capacity = 0;
    int slots_in = -1;
    float heat_ratio = 0.0f;
    size_t inventory_bytes = 0;
    int hatch_count = -1;
    double steam_current = -1.0;
    double steam_capacity = -1.0;
};

struct RecordingPublisher : simcore::IEventPublisher {
    std::vector<PublisherEvent> updates;

    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char*, uint32_t, uint8_t) override {}

    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t, uint8_t) override {}

    void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                  uint32_t, uint64_t) override {}

    void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z,
                                  uint16_t machine_type,
                                  const std::vector<uint8_t>& inventory_data,
                                  float progress, uint32_t energy,
                                  EnergyType energy_type, uint32_t energy_capacity,
                                  int slots_in, float heat_ratio,
                                  const std::vector<HatchUpdateData>* hatches = nullptr,
                                  double steam_current = -1.0,
                                  double steam_capacity = -1.0) override {
        PublisherEvent e;
        e.x = x; e.y = y; e.z = z;
        e.machine_id = machine_type;
        e.progress = progress;
        e.energy = energy;
        e.energy_type = energy_type;
        e.energy_capacity = energy_capacity;
        e.slots_in = slots_in;
        e.heat_ratio = heat_ratio;
        e.inventory_bytes = inventory_data.size();
        e.hatch_count = hatches ? static_cast<int>(hatches->size()) : -1;
        e.steam_current = steam_current;
        e.steam_capacity = steam_capacity;
        updates.push_back(e);
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
// Constants mirrored from production (private / in another header's namespace)
// ---------------------------------------------------------------------------

constexpr int kConversionRate = simcore::HeatConstants::CONVERSION_RATE;      // 1
constexpr int kReplenishTarget = simcore::HeatConstants::HEAT_SINK_REPLENISH_TARGET;  // 100
constexpr int kBoilerHeatPerFuel = simcore::LargeBoilerSystem::BOILER_HEAT_PER_FUEL;  // 100
constexpr int kSteamPerWater = simcore::LargeBoilerSystem::STEAM_PER_WATER;   // 10
constexpr uint16_t kCoalId = simcore::LargeBoilerSystem::COAL_BLOCK_ID;       // 1010
constexpr uint16_t kCharcoalId = simcore::LargeBoilerSystem::CHARCOAL_BLOCK_ID;  // 1011
constexpr uint16_t kBoilerId = content::kBoilerMachineId;                     // 1110:011:1

// A non-zero Steam item id stands in for the registry-resolved id that
// main.cpp:104 passes as `steam_item_id`. 0 means "registry unavailable", which
// the boiler fails closed on (BoilerSystem.cpp:50).
constexpr uint16_t kSteamItemId = ItemId::pack("0:1110:004:64");

// ---------------------------------------------------------------------------
// Fixture 1: BoilerSystem (heat boiler)
// ---------------------------------------------------------------------------

struct HeatBoilerFixture {
    entt::registry reg;
    std::shared_ptr<RecordingPublisher> events;
    std::unique_ptr<simcore::BoilerSystem> sys;
};

static std::shared_ptr<HeatBoilerFixture> makeHeatBoiler(uint16_t steam_item_id = kSteamItemId) {
    auto f = std::make_shared<HeatBoilerFixture>();
    f->events = std::make_shared<RecordingPublisher>();
    // Null pipe, fluid, port and state clients: every call site is guarded, so
    // the conversion path runs with no router, socket or wall clock.
    f->sys = std::make_unique<simcore::BoilerSystem>(
        f->reg, f->events, nullptr /* pipeClient */, nullptr /* fluidClient */,
        nullptr /* portClient */, steam_item_id, nullptr /* statePublisher */);
    return f;
}

struct HeatBoilerRig {
    entt::entity entity = entt::null;
};

// A boiler entity needs ALL FIVE components the tick's view requires:
// MachineComponent, EnergyStorage, HeatIntakeComponent, SteamOutputComponent.
static HeatBoilerRig installHeatBoiler(HeatBoilerFixture& f, uint32_t x, uint32_t y,
                                        uint32_t z, int32_t energy_current,
                                        int32_t heat_stored, double steam_stored,
                                        double steam_capacity = 1000.0) {
    HeatBoilerRig rig;
    auto ent = f.reg.create();
    f.reg.emplace<simcore::Position>(ent, x, y, z);
    f.reg.emplace<simcore::MachineComponent>(ent, kBoilerId, 0, x, y, z, 0);
    f.reg.emplace<simcore::EnergyStorage>(ent, 10000, energy_current, 0, 0, 0,
                                          EnergyType::HEAT);
    f.reg.emplace<simcore::HeatIntakeComponent>(ent);
    f.reg.get<simcore::HeatIntakeComponent>(ent).heat_stored = heat_stored;
    f.reg.emplace<simcore::SteamOutputComponent>(ent);
    auto& steam = f.reg.get<simcore::SteamOutputComponent>(ent);
    steam.steam_stored = steam_stored;
    steam.steam_capacity = steam_capacity;
    rig.entity = ent;
    return rig;
}

// ---------------------------------------------------------------------------
// Fixture 2: LargeBoilerSystem (solid fuel)
// ---------------------------------------------------------------------------

struct LargeBoilerFixture {
    entt::registry reg;
    std::unordered_map<uint64_t, simcore::MultiblockController> controllers;
    simcore::PatternRegistry patterns;
    std::shared_ptr<RecordingPublisher> events;
    std::unique_ptr<simcore::LargeBoilerSystem> sys;
    static std::vector<std::string>& tempFiles() {
        static std::vector<std::string> files;
        return files;
    }
};

// Pattern 2 is the large boiler. LargeBoilerSystem::tick bails out when
// patterns_.getPattern(2) is null, so a one-layer pattern must be registered in
// the FIXTURE (the system holds the registry by reference, so the caller must
// own it for the fixture's whole lifetime).
static simcore::MultiblockPattern makeLargeBoilerPattern() {
    simcore::MultiblockPattern pattern;
    pattern.id = 2;
    pattern.name = "large_boiler";
    pattern.size_x = 1;
    pattern.size_y = 1;
    pattern.size_z = 1;
    pattern.layers = {simcore::PatternLayer{}};
    return pattern;
}

// Returns a shared_ptr, NOT a Fixture by value: LargeBoilerSystem stores a
// reference to the fixture's registry / controller map, so the fixture must
// never move after construction.
static std::shared_ptr<LargeBoilerFixture> makeLargeBoiler() {
    auto f = std::make_shared<LargeBoilerFixture>();
    f->patterns.addPattern(makeLargeBoilerPattern());
    f->events = std::make_shared<RecordingPublisher>();
    f->sys = std::make_unique<simcore::LargeBoilerSystem>(
        f->reg, f->controllers, f->patterns, f->events,
        nullptr /* pipeClient */, nullptr /* itemClient */);
    return f;
}

struct LargeRig {
    entt::entity entity = entt::null;
    uint64_t controller_id = 0;
};

static LargeRig installLargeBoiler(LargeBoilerFixture& f, uint32_t x, uint32_t y,
                                   uint32_t z, uint32_t pattern_id, int slots,
                                   int32_t energy_capacity, int32_t energy_current,
                                   int32_t max_input) {
    LargeRig rig;
    rig.controller_id = f.controllers.size() + 1;
    f.controllers.emplace(rig.controller_id,
                          simcore::MultiblockController(rig.controller_id, x, y, z,
                                                        pattern_id,
                                                        std::vector<uint32_t>{}));
    auto ent = f.reg.create();
    f.reg.emplace<simcore::Position>(ent, x, y, z);
    f.reg.emplace<simcore::MachineComponent>(ent, kBoilerId, rig.controller_id, x, y, z,
                                             rig.controller_id);
    f.reg.emplace<simcore::EnergyStorage>(ent, energy_capacity, energy_current,
                                          max_input, 0, 0, EnergyType::STEAM);
    f.reg.emplace<simcore::RecipeProgress>(ent);
    simcore::InventoryContainer container(0, static_cast<uint16_t>(slots),
                                          std::vector<simcore::InventorySlot>{});
    f.reg.emplace<simcore::InventoryContainer>(ent, container);
    rig.entity = ent;
    return rig;
}

static void putFuel(LargeBoilerFixture& f, entt::entity ent, int slot, uint16_t item_id,
                    uint8_t count) {
    f.reg.get<simcore::InventoryContainer>(ent).slots[slot] = {item_id, count, 0};
}

// Returns 0 when the boiler has no HeatIntakeComponent yet. The component is
// get_or_emplace'd by the tick, so an untouched boiler legitimately has none.
static int32_t heatOf(LargeBoilerFixture& f, entt::entity ent) {
    const auto* heat = f.reg.try_get<simcore::HeatIntakeComponent>(ent);
    return heat ? heat->heat_stored : 0;
}

// ---------------------------------------------------------------------------
// BoilerSystem: conversion
// ---------------------------------------------------------------------------

static void test_boiler_conversion_is_one_to_one() {
    // 5 HU in, exactly 5 steam out (CONVERSION_RATE caps each tick at 1, so a
    // single tick converts 1 HU; five ticks convert 5).
    auto f = makeHeatBoiler();
    auto rig = installHeatBoiler(*f, 50, 70, 50, 5, 5, 0.0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 4,
                 "one tick converts 1 HU out of heat_stored");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 4,
                 "the same 1 HU comes out of the energy mirror");
    const auto& steam = f->reg.get<simcore::SteamOutputComponent>(rig.entity);
    CHECK(steam.steam_stored == 1.0, "1 steam is produced: HU in == steam out");

    for (int i = 0; i < 4; ++i) f->sys->tick(0.05f);
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 0,
                 "five ticks drain the five HU");
    CHECK(f->reg.get<simcore::SteamOutputComponent>(rig.entity).steam_stored == 5.0,
          "and produce exactly five steam");
}

static void test_boiler_conversion_rate_caps_each_tick() {
    // 1000 HU available but only 1 steam per tick, so the rate is the binding
    // constraint, not the buffer.
    auto f = makeHeatBoiler();
    auto rig = installHeatBoiler(*f, 51, 70, 51, 1000, 1000, 0.0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 999,
                 "a full buffer still converts only 1 HU per tick");
    CHECK(f->reg.get<simcore::SteamOutputComponent>(rig.entity).steam_stored == 1.0,
          "CONVERSION_RATE (1) is the per-tick ceiling");
}

static void test_boiler_steam_full_stops_conversion() {
    auto f = makeHeatBoiler();
    auto rig = installHeatBoiler(*f, 52, 70, 52, 500, 500, 999.0, 1000.0);

    f->sys->tick(0.05f);

    CHECK(f->reg.get<simcore::SteamOutputComponent>(rig.entity).steam_stored == 1000.0,
          "a boiler with 1 slot of steam room fills it and stops");
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 499,
                 "exactly the 1 HU of steam room was consumed");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 499,
                 "and the energy mirror follows");

    f->sys->tick(0.05f);
    CHECK(f->reg.get<simcore::SteamOutputComponent>(rig.entity).steam_stored == 1000.0,
          "a full boiler converts nothing further");
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 499,
                 "and keeps its heat, which is what a steam-full boiler must do");
}

static void test_boiler_cold_boiler_converts_nothing() {
    auto f = makeHeatBoiler();
    auto rig = installHeatBoiler(*f, 53, 70, 53, 0, 0, 0.0);

    f->sys->tick(0.05f);

    CHECK(f->reg.get<simcore::SteamOutputComponent>(rig.entity).steam_stored == 0.0,
          "no heat in, no steam out");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 0,
                 "and the buffer is untouched");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                 "a cold boiler still publishes, or the client hides the bar");
}

static void test_boiler_zero_steam_id_fails_closed() {
    // A zero steam_item_id means the registry could not resolve the Steam item,
    // so the produced steam could never be drained. The system must not convert.
    auto f = makeHeatBoiler(/*steam_item_id=*/0);
    auto rig = installHeatBoiler(*f, 54, 70, 54, 500, 500, 0.0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 500,
                 "with no Steam id the heat is NOT converted");
    CHECK(f->reg.get<simcore::SteamOutputComponent>(rig.entity).steam_stored == 0.0,
          "and no steam is produced");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                 "but the state is still published");
}

static void test_boiler_heat_mirror_is_a_max_not_a_set() {
    // BoilerSystem.cpp:46-48 does `heat_stored = max(heat_stored, current)`.
    // A MAX means a stale HeatIntakeComponent can never be lowered by a cooler
    // energy buffer: heat sticks at the higher of the two forever.
    auto f = makeHeatBoiler();
    // heat_stored is deliberately HIGHER than the energy buffer.
    auto rig = installHeatBoiler(*f, 55, 70, 55, /*energy_current=*/10,
                                 /*heat_stored=*/900, 0.0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 899,
                 "the max keeps the higher heat_stored (900), converting 1 HU");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 9,
                 "while the colder energy buffer is debited to 9");
    CHECK(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored >
              f->reg.get<simcore::EnergyStorage>(rig.entity).current,
          "the two fields now disagree by 890 HU and stay that way");
}

static void test_boiler_heat_mirror_raises_a_cold_intake() {
    // The same max in the other direction: a hot energy buffer raises a cold
    // heat_stored, so the boiler can convert heat it never received via
    // HeatIntakeComponent.
    auto f = makeHeatBoiler();
    auto rig = installHeatBoiler(*f, 56, 70, 56, /*energy_current=*/300,
                                 /*heat_stored=*/0, 0.0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 299,
                 "the max raised heat_stored to the energy buffer's 300, then -1");
    CHECK(f->reg.get<simcore::SteamOutputComponent>(rig.entity).steam_stored == 1.0,
          "so one steam is produced from a 'cold' intake component");
}

static void test_boiler_ignores_non_boiler_machine_ids() {
    auto f = makeHeatBoiler();
    auto rig = installHeatBoiler(*f, 57, 70, 57, 500, 500, 0.0);
    f->reg.get<simcore::MachineComponent>(rig.entity).machine_id = 0xDEAD;

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 500,
                 "a machine with a different id is skipped entirely");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "and nothing is published");
}

static void test_boiler_incomplete_component_set_is_skipped() {
    // The tick's view requires all four components. An entity missing
    // SteamOutputComponent is invisible to it — no crash, no publish.
    auto f = makeHeatBoiler();
    auto ent = f->reg.create();
    f->reg.emplace<simcore::Position>(ent, 58, 70, 58);
    f->reg.emplace<simcore::MachineComponent>(ent, kBoilerId, 0, 58, 70, 58, 0);
    f->reg.emplace<simcore::EnergyStorage>(ent, 1000, 500, 0, 0, 0, EnergyType::HEAT);
    f->reg.emplace<simcore::HeatIntakeComponent>(ent);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->events->updates.size(), size_t(0),
                 "a boiler without SteamOutputComponent is skipped without crashing");
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(ent).heat_stored, 0,
                 "and is left completely untouched");
}

static void test_boiler_publishes_zero_capacity() {
    // FINDING E: the publish passes a LITERAL 0 for energy_capacity
    // (BoilerSystem.cpp:74), so MachineWindow.cpp:426-428's tier*10000
    // fallback wins and every boiler shows a 10000-unit bar.
    auto f = makeHeatBoiler();
    installHeatBoiler(*f, 59, 70, 59, 500, 500, 0.0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->events->updates.size(), size_t(1), "one update published");
    CHECK_EQ_INT(f->events->updates[0].energy_capacity, 0u,
                 "energy_capacity is a literal 0, so the client's tier*10000 "
                 "fallback applies");
    CHECK_EQ_INT(f->events->updates[0].energy_type, EnergyType::HEAT,
                 "the energy type is HEAT");
    // heat_ratio is read from the component AFTER the conversion, so it is
    // 499/1000 — the post-tick level, not the seeded 500/1000.
    CHECK(fabs(f->events->updates[0].heat_ratio - 0.499f) < 1e-5f,
          "heat_ratio is the component's ratio read AFTER the conversion (499/1000)");
    CHECK_EQ_INT(f->events->updates[0].slots_in, -1,
                 "slots_in is -1: the boiler has no item grid");
}

static void test_boiler_publishes_steam_levels() {
    // Unlike the large boiler, this one reports the steam buffer as the
    // steam_current/steam_capacity tail arguments.
    auto f = makeHeatBoiler();
    installHeatBoiler(*f, 60, 70, 60, 300, 300, 250.0, 1000.0);

    f->sys->tick(0.05f);

    const auto& e = f->events->updates[0];
    CHECK(e.steam_current == 251.0, "steam_current is the post-conversion level");
    CHECK(e.steam_capacity == 1000.0, "steam_capacity is the buffer's capacity");
}

static void test_boiler_replace_port_returns_monotonic_epochs() {
    auto f = makeHeatBoiler();
    // replacePort is the only public mutator besides tick; epochs are per-port
    // and monotonic, so a repeated replace keeps increasing.
    // PortEpochBook::Replace is epoch + 1 from kFirstEpoch == 1, so the first
    // replace on ANY port returns 2 (BoilerPorts.h:99-103). Epochs are keyed on
    // (owner_id, port_id), so different ports are INDEPENDENT — which is why
    // both first replaces return the same 2 rather than colliding.
    const std::uint64_t e0 = f->sys->replacePort(42, 0);
    const std::uint64_t e1 = f->sys->replacePort(42, 0);
    const std::uint64_t other = f->sys->replacePort(42, 1);
    CHECK_EQ_INT(e0, 2u, "the first replace on a port returns kFirstEpoch + 1");
    CHECK_EQ_INT(e1, 3u, "a second replace on the same port bumps the epoch");
    CHECK_EQ_INT(other, 2u,
                 "a different port has its own epoch space, so it is unaffected "
                 "by the first port's replaces");
    // A different OWNER is likewise independent.
    const std::uint64_t other_owner = f->sys->replacePort(43, 0);
    CHECK_EQ_INT(other_owner, 2u,
                 "epochs are keyed on (owner, port), so a new owner restarts at 2");
}

static void test_boiler_multiple_boilers_all_convert() {
    auto f = makeHeatBoiler();
    auto a = installHeatBoiler(*f, 61, 70, 61, 10, 10, 0.0);
    auto b = installHeatBoiler(*f, 62, 70, 62, 10, 10, 0.0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(a.entity).heat_stored, 9,
                 "boiler A converted");
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(b.entity).heat_stored, 9,
                 "boiler B converted");
    CHECK_EQ_INT(f->events->updates.size(), size_t(2), "each publishes its own update");
}

// ---------------------------------------------------------------------------
// LargeBoilerSystem: fuel burn, steam saturation, overheat
// ---------------------------------------------------------------------------

static void test_large_boiler_coal_burns_into_heat_and_steam() {
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 70, 70, 70, 2, 2, 1000, 0, 32);
    putFuel(*f, rig.entity, 0, kCoalId, 1);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel,
                 "one coal adds 100 HU");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, kSteamPerWater,
                 "one coal adds 10 steam");
    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[0].item_id, 0, "the coal is consumed");
}

static void test_large_boiler_charcoal_is_also_fuel() {
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 71, 70, 71, 2, 2, 1000, 0, 32);
    putFuel(*f, rig.entity, 1, kCharcoalId, 1);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel, "charcoal burns like coal");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, kSteamPerWater,
                 "and makes the same steam");
}

static void test_large_boiler_non_fuel_item_is_not_burned() {
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 72, 70, 72, 2, 2, 1000, 0, 32);
    putFuel(*f, rig.entity, 0, 0x1234, 5);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), 0, "a non-fuel item makes no heat");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].count, 5,
                 "and is not consumed");
}

static void test_large_boiler_steam_yield_is_truncated_by_max_input() {
    // FINDING A: addEnergy() clamps to maxInput (EnergyStorage.h:56-58), so
    // STEAM_PER_WATER (10) is silently truncated. With maxInput == 1 only ONE
    // steam is produced per fuel, not the ten the constant documents.
    // produceEnergy() — which exists exactly for producers and is documented
    // "production is not limited by maxInput" — is not used here.
    for (int32_t max_input : {1, 3, 5, 10, 32}) {
        auto f = makeLargeBoiler();
        auto rig = installLargeBoiler(*f, 73, 70, 73, 2, 2, 1000, 0, max_input);
        putFuel(*f, rig.entity, 0, kCoalId, 1);

        f->sys->tick(0.05f);

        const int32_t expected = std::min(max_input, kSteamPerWater);
        CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, expected,
                     "the yield is min(maxInput, STEAM_PER_WATER), not STEAM_PER_WATER");
    }
}

static void test_large_boiler_zero_max_input_produces_no_steam() {
    // The default EnergyStorage has maxInput == 0, and addEnergy(10) against
    // maxInput 0 accepts NOTHING. The boiler burns the coal, makes the heat and
    // produces zero steam — the fuel is destroyed.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 74, 70, 74, 2, 2, 1000, 0, 0);
    putFuel(*f, rig.entity, 0, kCoalId, 1);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel, "the heat is still made");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 0,
                 "but with maxInput == 0 addEnergy accepts nothing and the steam is lost");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].item_id, 0,
                 "and the coal is gone regardless");
}

static void test_large_boiler_steam_saturates_at_capacity() {
    // Saturation: a nearly full buffer accepts only the room it has.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 75, 70, 75, 2, 2, 100, 98, 32);
    putFuel(*f, rig.entity, 0, kCoalId, 1);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 100,
                 "only the 2 units of room were taken; the other 8 are dropped");
    CHECK(f->reg.get<simcore::EnergyStorage>(rig.entity).isFull(),
          "the buffer is now exactly full");
}

static void test_large_boiler_full_steam_buffer_still_burns_fuel() {
    // Saturation does NOT gate the burn: a full steam buffer still consumes the
    // coal and makes the heat, and only the steam is lost.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 76, 70, 76, 2, 2, 100, 100, 32);
    putFuel(*f, rig.entity, 0, kCoalId, 1);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel,
                 "the heat is produced even though the steam buffer is full");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 100,
                 "the steam is unchanged");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].item_id, 0,
                 "and the fuel is still consumed — heat for nothing");
}

static void test_large_boiler_heat_is_capped_but_energy_is_not_mirrored() {
    // FINDING B: heat_stored is clamped to heat_capacity (default 1000) but the
    // EnergyStorage mirror is never written, so the two fields drift apart and
    // never resynchronise. Pinned over several fuel units.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 77, 70, 77, 2, 2, 100000, 0, 1000);
    putFuel(*f, rig.entity, 0, kCoalId, 20);

    // 20 coal x 100 HU = 2000 HU, which is twice the default 1000 HU capacity.
    for (int i = 0; i < 20; ++i) f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), 1000,
                 "heat saturates at the component capacity of 1000 HU");
    CHECK(f->reg.get<simcore::EnergyStorage>(rig.entity).current > 0,
          "while the energy buffer is nowhere near its own capacity");
    CHECK(f->reg.get<simcore::EnergyStorage>(rig.entity).current != 1000,
          "the two are never brought into agreement: no mirror is written back");
}

static void test_large_boiler_cooling_is_five_per_tick_not_dt_scaled() {
    // FINDING D: the tick signature is `tick(float)` with the argument unnamed
    // and unused, and the cooling step is a hard `min(5, heat_stored)`. So the
    // boiler cools 5 HU per TICK no matter what dt is.
    for (float dt : {0.001f, 0.05f, 10.0f}) {
        auto f = makeLargeBoiler();
        auto rig = installLargeBoiler(*f, 78, 70, 78, 2, 2, 1000, 0, 32);
        f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
        f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 50;

        f->sys->tick(dt);

        CHECK_EQ_INT(heatOf(*f, rig.entity), 45,
                     "cooling is a flat 5 HU per tick, identical for every dt");
    }
}

static void test_large_boiler_cooling_never_goes_negative() {
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 79, 70, 79, 2, 2, 1000, 0, 32);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 3;

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), 0, "the last 3 HU cool to exactly 0, not -2");
    f->sys->tick(0.05f);
    CHECK_EQ_INT(heatOf(*f, rig.entity), 0, "and stay at 0 rather than going negative");
}

static void test_large_boiler_overheat_warning_requires_the_no_fuel_path() {
    // FINDING C: the OverheatComponent is emplaced ONLY in the `else`
    // (no-fuel) branch, so a boiler that is at 100% heat but still burning fuel
    // never warns.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 80, 70, 80, 2, 2, 1000, 0, 32);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 1000;
    putFuel(*f, rig.entity, 0, kCoalId, 1);

    f->sys->tick(0.05f);

    CHECK(!f->reg.all_of<simcore::OverheatComponent>(rig.entity),
          "a boiler at 100% heat that is BURNING fuel does not raise a warning");

    // The same boiler, out of fuel, does warn.
    putFuel(*f, rig.entity, 0, kCoalId, 0);
    f->sys->tick(0.05f);
    f->sys->tick(0.05f);
    f->sys->tick(0.05f);
    CHECK(f->reg.all_of<simcore::OverheatComponent>(rig.entity),
          "the warning needs the no-fuel path");
    CHECK(f->reg.get<simcore::OverheatComponent>(rig.entity).state ==
              simcore::OverheatState::WARNING,
          "and the state is WARNING, not CRITICAL, however hot the boiler is");
}

static void test_large_boiler_fuel_clears_an_existing_overheat() {
    // The converse: once fuel returns, the component is removed outright.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 81, 70, 81, 2, 2, 1000, 0, 32);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 1000;
    f->reg.emplace<simcore::OverheatComponent>(rig.entity);

    f->sys->tick(0.05f);  // no fuel yet -> warning path

    CHECK(f->reg.all_of<simcore::OverheatComponent>(rig.entity), "warning is set");

    putFuel(*f, rig.entity, 0, kCoalId, 1);
    f->sys->tick(0.05f);

    CHECK(!f->reg.all_of<simcore::OverheatComponent>(rig.entity),
          "burning fuel removes the overheat component entirely");
}

static void test_large_boiler_publishes_steam_type_and_ratio() {
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 82, 70, 82, 2, 2, 1000, 0, 32);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 500;

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->events->updates.size(), size_t(1), "one update published");
    const auto& e = f->events->updates[0];
    CHECK_EQ_INT(e.energy_type, EnergyType::STEAM, "the large boiler reports STEAM");
    CHECK_EQ_INT(e.energy, 0u, "the published energy is a literal 0, not the buffer");
    CHECK_EQ_INT(e.energy_capacity, 0u,
                 "and the capacity is a literal 0, so the client falls back to tier*10000");
    CHECK_EQ_INT(e.progress, 0.0f, "the large boiler has no recipe progress");
    CHECK_EQ_INT(e.slots_in, -1, "and no input grid");
    // 500 HU cooled to 495 on this no-fuel tick, so the ratio is 495/1000.
    CHECK(fabs(e.heat_ratio - 0.495f) < 1e-5f,
          "heat_ratio is the component's ratio read AFTER the 5 HU cooling step");
    CHECK(e.steam_current == -1.0,
          "unlike the heat boiler, no steam levels are published in the tail args");
}

static void test_large_boiler_zero_key_is_skipped() {
    // LargeBoilerSystem.cpp:30-31 binds `ctrl_id` from the MAP KEY, so only a
    // key of 0 is skipped. Setting MultiblockController::id to 0 does NOT skip
    // the controller — a distinct quirk from EBFSystem and LCRSystem, which
    // both read ctrl.id. Pinned here as observed.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 83, 70, 83, 2, 2, 1000, 0, 32);
    putFuel(*f, rig.entity, 0, kCoalId, 1);

    // ctrl.id == 0 does NOT skip: the boiler still burns.
    f->controllers[rig.controller_id].id = 0;
    f->sys->tick(0.05f);
    CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel,
                 "setting MultiblockController::id to 0 does NOT skip the controller: "
                 "the guard reads the map key, not ctrl.id");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].item_id, 0,
                 "and the coal is burned anyway");
}

static void test_large_boiler_zero_map_key_is_skipped() {
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 89, 70, 89, 2, 2, 1000, 0, 32);
    putFuel(*f, rig.entity, 0, kCoalId, 1);
    // Re-key the controller map to 0 — this is what the guard actually reads.
    f->controllers.clear();
    f->controllers.emplace(0, simcore::MultiblockController(1, 89, 70, 89, 2,
                                                            std::vector<uint32_t>{}));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), 0,
                 "a map KEY of 0 is skipped (LargeBoilerSystem.cpp:31)");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].count, 1,
                 "and the coal is not burned");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "nothing is published");
}

static void test_large_boiler_pattern_id_is_not_checked() {
    // tick() only requires patterns_.getPattern(2) to EXIST; it never compares
    // the controller's pattern_id against 2 (contrast LCRSystem, which does the
    // same, and the EBF, which whitelists {1, 4}).
    for (uint32_t pattern : {0u, 1u, 2u, 3u, 77u}) {
        auto f = makeLargeBoiler();
        auto rig = installLargeBoiler(*f, 84, 70, 84, pattern, 2, 1000, 0, 32);
        putFuel(*f, rig.entity, 0, kCoalId, 1);
        f->sys->tick(0.05f);
        CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel,
                     "any pattern_id is burned as a large boiler");
    }
}

static void test_large_boiler_pattern_2_is_always_present() {
    // The `if (!pattern) continue;` guard at LargeBoilerSystem.cpp:33 cannot be
    // reached through the public API: PatternRegistry's DEFAULT CONSTRUCTOR
    // already registers pattern 2 (PatternLibrary.cpp:156-157), so every
    // registry the production wiring can pass already contains it. The guard is
    // defensive-only. Pinned as observed so a future PatternLibrary change that
    // drops the large boiler pattern would show up here.
    auto f = makeLargeBoiler();
    CHECK(f->patterns.getPattern(2) != nullptr,
          "a default-constructed PatternRegistry already contains pattern 2");
    CHECK(f->patterns.getPattern(2)->name == "large_boiler",
          "and it is the large boiler");

    // The guard is checked once per controller, before the entity lookup, so a
    // valid registry means every controller reaches tickBoiler.
    auto rig = installLargeBoiler(*f, 85, 70, 85, 2, 2, 1000, 0, 32);
    putFuel(*f, rig.entity, 0, kCoalId, 1);
    f->sys->tick(0.05f);
    CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel,
                 "with the default registry the controller is always ticked");
}

static void test_large_boiler_controller_without_entity_is_skipped() {
    auto f = makeLargeBoiler();
    const uint64_t id = 55;
    f->controllers.emplace(id, simcore::MultiblockController(id, 900, 800, 700, 2,
                                                              std::vector<uint32_t>{}));
    f->sys->tick(0.05f);
    CHECK_EQ_INT(f->events->updates.size(), size_t(0),
                 "a controller with no matching entity is a safe no-op");
}

static void test_large_boiler_get_or_emplace_creates_the_heat_component() {
    // heatIntake is get_or_emplace'd, so a controller with no HeatIntake
    // component still ticks — the component appears with its defaults.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 86, 70, 86, 2, 2, 1000, 0, 32);
    CHECK(!f->reg.all_of<simcore::HeatIntakeComponent>(rig.entity),
          "the entity starts with no heat component");

    f->sys->tick(0.05f);

    CHECK(f->reg.all_of<simcore::HeatIntakeComponent>(rig.entity),
          "the tick creates it via get_or_emplace");
    CHECK_EQ_INT(heatOf(*f, rig.entity), 0, "with heat_stored defaulting to 0");
}

static void test_large_boiler_picks_the_first_fuel_slot() {
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 87, 70, 87, 2, 4, 1000, 0, 32);
    putFuel(*f, rig.entity, 3, kCoalId, 2);  // only the last slot has fuel

    f->sys->tick(0.05f);

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[3].count, 1, "exactly one coal is burned per tick");
    CHECK_EQ_INT(heatOf(*f, rig.entity), kBoilerHeatPerFuel, "giving 100 HU");
}

static void test_large_boiler_empty_fuel_slot_is_not_burned() {
    // A coal with count == 0 is skipped: the scan requires count > 0, so a
    // stale item_id in an empty slot cannot fuel the boiler.
    auto f = makeLargeBoiler();
    auto rig = installLargeBoiler(*f, 88, 70, 88, 2, 2, 1000, 0, 32);
    putFuel(*f, rig.entity, 0, kCoalId, 0);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(heatOf(*f, rig.entity), 0, "an empty coal slot makes no heat");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].item_id,
                 kCoalId, "and the stale id is left in place");
}



#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== boiler_system test suite ===\n\n");

    // BoilerSystem: HEAT -> STEAM conversion
    TEST(boiler_conversion_is_one_to_one);
    TEST(boiler_conversion_rate_caps_each_tick);
    TEST(boiler_steam_full_stops_conversion);
    TEST(boiler_cold_boiler_converts_nothing);
    TEST(boiler_zero_steam_id_fails_closed);
    TEST(boiler_heat_mirror_is_a_max_not_a_set);
    TEST(boiler_heat_mirror_raises_a_cold_intake);
    TEST(boiler_ignores_non_boiler_machine_ids);
    TEST(boiler_incomplete_component_set_is_skipped);
    TEST(boiler_publishes_zero_capacity);
    TEST(boiler_publishes_steam_levels);
    TEST(boiler_replace_port_returns_monotonic_epochs);
    TEST(boiler_multiple_boilers_all_convert);

    // LargeBoilerSystem: fuel burn, steam saturation, overheat
    TEST(large_boiler_coal_burns_into_heat_and_steam);
    TEST(large_boiler_charcoal_is_also_fuel);
    TEST(large_boiler_non_fuel_item_is_not_burned);
    TEST(large_boiler_steam_yield_is_truncated_by_max_input);
    TEST(large_boiler_zero_max_input_produces_no_steam);
    TEST(large_boiler_steam_saturates_at_capacity);
    TEST(large_boiler_full_steam_buffer_still_burns_fuel);
    TEST(large_boiler_heat_is_capped_but_energy_is_not_mirrored);
    TEST(large_boiler_cooling_is_five_per_tick_not_dt_scaled);
    TEST(large_boiler_cooling_never_goes_negative);
    TEST(large_boiler_overheat_warning_requires_the_no_fuel_path);
    TEST(large_boiler_fuel_clears_an_existing_overheat);
    TEST(large_boiler_publishes_steam_type_and_ratio);
    TEST(large_boiler_zero_key_is_skipped);
    TEST(large_boiler_zero_map_key_is_skipped);
    TEST(large_boiler_pattern_id_is_not_checked);
    TEST(large_boiler_pattern_2_is_always_present);
    TEST(large_boiler_controller_without_entity_is_skipped);
    TEST(large_boiler_get_or_emplace_creates_the_heat_component);
    TEST(large_boiler_picks_the_first_fuel_slot);
    TEST(large_boiler_empty_fuel_slot_is_not_burned);

    for (const auto& path : LargeBoilerFixture::tempFiles()) remove(path.c_str());
    LargeBoilerFixture::tempFiles().clear();

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
