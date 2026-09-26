// LCRSystem unit tests (issue gp-mb2d).
//
// Covers src/game/machines/LCRSystem.cpp — the Large Chemical Reactor. The
// openspec task (add-scaled-energy-hatch-multiblock-verification 1.3) asks for
// "LCR ENERGY hatch detection, absent hatch rejection/fallback, tier, and face
// configuration". This suite tests all four plus the execution path.
//
// ANSWER TO THE OPESPEC'S "REJECTION/FALLBACK" QUESTION: the code does
// FALLBACK, not rejection. LCRSystem.cpp:49-64 scans ctrl.hatches for the
// first HatchType::ENERGY hatch with present == true and, when none is found,
// falls back to the CONTROLLER'S OWN (ctrl.x, ctrl.y, ctrl.z) as the energy
// endpoint. This is the opposite of EBFSystem, which REQUIRES a physical
// ENERGY hatch and returns early without one (EBFSystem.cpp:159-161):
//
//     EBF:   const HatchSlot* energy_hatch = nullptr;
//            if (electric) { ...find...; if (!energy_hatch) return; }
//     LCR:   const HatchSlot* energy_hatch = nullptr;
//            for (...) { if (ENERGY && present) { energy_hatch = &hatch; break; } }
//            // no `if (!energy_hatch) return` anywhere
//
// Both behaviours are pinned below (test_LCRSystem_absent_energy_hatch_falls_back
// vs the EBF contrast documented in test_ebf_system.cpp).
//
// LIVENESS: like EBFSystem, LCRSystem is driven from the controllers_ map
// (main.cpp:500 hands it SimulationEngine::getControllers()), which
// SimulationEngine.cpp:446 populates. The machine entity is located by
// scanning reg_.view<const Position, MachineComponent>() for a Position equal
// to the CONTROLLER's coordinates (LCRSystem.cpp:65-74).
//
// IMPORTANT DIFFERENCE FROM EBFSystem: LCRSystem::tick() does NOT check the
// controller's pattern_id. It only requires that patterns_.getPattern(3) — the
// hardcoded LCR pattern — exists (LCRSystem.cpp:41-42). So a controller with
// ANY pattern id is ticked as an LCR as long as pattern 3 is registered.
// Pinned by test_LCRSystem_pattern_id_is_not_checked.
//
// CONSTANTS AND FIELDS ASSERTED BY NAME (so a change shows in the diff):
//   EBFSystem::KANHAL/NICHROME/TUNGSTENSTEEL_MAX_HEAT are EBF-only; the LCR has
//   no heat tier of its own and no coil scan.
//   LCR energy comes from EnergyStorage; the per-tick cost is
//     recipe->resourceAmountPerTick()  when the recipe has resource_requirements
//     recipe->energy_cost             otherwise
//   (LCRSystem.cpp:124-126).
//   The LCR's shipped recipes declare `resource_requirements: [{kind: EU,
//   amount: 32}]`, so the resourceAmountPerTick() branch is the one production
//   takes; the legacy energy_cost branch is pinned too, with a synthetic recipe.
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed" here):
//   A. THE LCR ORCHESTRATION GATE REQUIRES A NON-EU REQUIREMENT.
//      LCRSystem.cpp:117-123 sets orchestrated only when
//      `reservations_ && hasResourceRequirements() && has_non_eu_requirement`.
//      Every shipped chemical_reactor.yaml recipe declares EU-only
//      requirements, so has_non_eu_requirement is FALSE and the recipes take
//      the LEGACY energy_cost path — with no CraftReservationClient the
//      `eu:` field is 0, so perTickCost is 0 and the recipe runs FOR FREE.
//      Pinned by test_LCRSystem_eu_only_requirements_are_not_orchestrated.
//   B. THE EU-ONLY SENTINEL IS `consumed <= 0`, NOT "no response".
//      onConsumeResponse erases the pending entry and returns true for any
//      consumed <= 0, and returns FALSE only for an unknown node_id
//      (LCRSystem.cpp:306-317). Pinned by
//      test_LCRSystem_on_consume_response_contract.
//   C. THE LCR IS NOT TIER-GATED AT RUNTIME. findRecipeByInputs() filters on
//      min_tier <= machineTier <= max_tier using the tier the RecipeManager was
//      told at registration (main.cpp:233 registers the LCR controller at tier
//      0), NOT the tier in EnergyStorage. Pinned by
//      test_LCRSystem_tier_comes_from_the_registration_not_the_buffer.
//   D. THE PROGRESS VALUE IS PUBLISHED FROM the recipe's own duration and the
//      REMAINING tick count, so a 4-tick recipe publishes 0.25, 0.5, 0.75, 1.0
//      and then stops (recipe_id is cleared at completion). Pinned by
//      test_LCRSystem_progress_series_runs_to_one.
//   E. THE BLOCK-ENTITY UPDATE PUBLISHES energy.capacity, unlike
//      RotareGeneratorSystem (gp-bbbl). Asserted as working.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include "Network/IEventPublisher.h"
#include <common/ResourcePort.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/PatternLibrary.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/MultiblockController.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/RecipeProgress.h>
#include <game/machines/LCRSystem.h>
#include <game/recipes/RecipeManager.h>

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
// Test doubles
// ---------------------------------------------------------------------------

struct PublisherEvent {
    int32_t x = 0, y = 0, z = 0;
    uint16_t machine_id = 0;
    float progress = 0.0f;
    uint32_t energy = 0;
    uint32_t energy_capacity = 0;
    EnergyType energy_type = EnergyType::ELECTRICITY;
    int slots_in = -1;
    float heat_ratio = 0.0f;
    size_t inventory_bytes = 0;
    int hatch_count = -1;
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
                                  double = -1.0, double = -1.0) override {
        PublisherEvent e;
        e.x = x; e.y = y; e.z = z;
        e.machine_id = machine_type;
        e.progress = progress;
        e.energy = energy;
        e.energy_capacity = energy_capacity;
        e.energy_type = energy_type;
        e.slots_in = slots_in;
        e.heat_ratio = heat_ratio;
        e.inventory_bytes = inventory_data.size();
        e.hatch_count = hatches ? static_cast<int>(hatches->size()) : -1;
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
// Fixture
// ---------------------------------------------------------------------------

constexpr uint16_t kBronzeIngot = ItemId::pack("0:1110:001:51");
constexpr uint16_t kBronzePlate = ItemId::pack("0:1110:001:52");
constexpr uint16_t kLcrBlockId  = 0x0BEE;  // the LCR controller block id

// The synthetic recipe mirrors a shipped chemical_reactor.yaml entry: 4 ticks,
// EU-only resource_requirements of 32/tick, and NO `eu:` field — which is
// exactly the shape that makes finding A observable.
constexpr uint32_t kRecipeDuration = 4;
constexpr uint32_t kResourcePerTick = 32;
constexpr float kLegacyEuPerTick = 7.0f;  // a second recipe that uses `eu:`

static std::string scratchDir() {
    const char* tmp = getenv("TMPDIR");
    if (tmp && *tmp) {
        std::string dir(tmp);
        if (dir.back() != '/') dir += '/';
        return dir;
    }
    return "./";
}

struct Fixture {
    entt::registry reg;
    std::unordered_map<uint64_t, simcore::MultiblockController> controllers;
    simcore::PatternRegistry patterns;
    std::shared_ptr<RecipeManager::RecipeManager> recipes;
    std::shared_ptr<RecordingPublisher> events;
    std::unique_ptr<simcore::LCRSystem> sys;
    static std::vector<std::string>& tempFiles() {
        static std::vector<std::string> files;
        return files;
    }
};

static void writeSyntheticLCRRecipes() {
    const std::string path = scratchDir() + "lcr_synthetic_recipes.yaml";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fputs(
        "class: chemical_reactor\n"
        "recipes:\n"
        // Recipe 1: EU-only resource_requirements, no `eu:` field — the exact
        // shape every shipped chemical_reactor.yaml recipe has.
        "  - name: test_lcr_bronze_plate\n"
        "    inputs:\n"
        "      - { item: \"0:1110:001:51\", count: 1 }\n"
        "    outputs:\n"
        "      - { item: \"0:1110:001:52\", count: 1 }\n"
        "    duration: 4\n"
        "    resource_requirements:\n"
        "      - { kind: EU, amount: 32, tier: 0 }\n"
        "    min_tier: 0\n"
        "    max_tier: 32767\n"
        // Recipe 2: the LEGACY path — a real `eu:` and no requirements.
        "  - name: test_lcr_legacy\n"
        "    inputs:\n"
        "      - { item: \"0:1110:001:61\", count: 1 }\n"
        "    outputs:\n"
        "      - { item: \"0:1110:001:62\", count: 1 }\n"
        "    duration: 4\n"
        "    eu: 7.0\n"
        "    min_tier: 0\n"
        "    max_tier: 32767\n",
        f);
    fclose(f);
    Fixture::tempFiles().push_back(path);
}

constexpr uint16_t kLegacyInput  = ItemId::pack("0:1110:001:61");
constexpr uint16_t kLegacyOutput = ItemId::pack("0:1110:001:62");

static void setupRecipes() {
    static bool done = false;
    if (done) return;
    writeSyntheticLCRRecipes();
    done = true;
}

// MachineRegistry has a private constructor; LoadFromYaml on an unopenable path
// yields an EMPTY but non-null registry, which is what the LCR's unguarded
// `MachineRegistry::instance()->Get(...)` (LCRSystem.cpp:95,100) requires.
static void installEmptyRegistry() {
    auto reg = MachineRegistry::LoadFromYaml("/nonexistent/machines.yaml");
    if (!reg) return;
    MachineRegistry::setInstance(reg.release());
}

static void installRegistry(uint16_t block_id, int slots_in, int slots_out) {
    auto reg = MachineRegistry::LoadFromYaml("/nonexistent/machines.yaml");
    if (!reg) return;
    MachineInfo info{};
    info.id = block_id;
    info.name = "large_chemical_reactor";
    info.machine_class = "chemical_reactor";
    info.energy_in = EnergyType::ELECTRICITY;
    info.tier = 1;
    info.slots_in = slots_in;
    info.slots_out = slots_out;
    info.capacity = 10000;
    info.maxInput = 32;
    info.maxOutput = 0;
    reg->Register(info);
    MachineRegistry::setInstance(reg.release());
}

// Returns a shared_ptr, NOT a Fixture by value: LCRSystem stores references to
// the fixture's registry / controller map, so the fixture must never move after
// construction. std::make_shared builds it in place.
static std::shared_ptr<Fixture> makeFixture() {
    setupRecipes();
    installEmptyRegistry();  // required: LCRSystem.cpp:95 dereferences instance()
    auto f = std::make_shared<Fixture>();
    f->recipes = std::make_shared<RecipeManager::RecipeManager>();
    f->recipes->loadRecipesFromYamlFile(scratchDir() + "lcr_synthetic_recipes.yaml");
    // Mirrors main.cpp:233 — the LCR controller is registered as class
    // "chemical_reactor" at tier 0 with an ELECTRICITY energy_in filter.
    f->recipes->registerMachineClass(
        kLcrBlockId, "chemical_reactor", 0,
        static_cast<uint8_t>(RecipeManager::EnergyType::ELECTRICITY));
    f->events = std::make_shared<RecordingPublisher>();
    f->sys = std::make_unique<simcore::LCRSystem>(f->reg, f->controllers, f->patterns,
                                                  f->recipes, f->events,
                                                  nullptr /* PipeEnergyClient */);
    return f;
}

static simcore::MultiblockController makeController(uint64_t id, uint32_t x, uint32_t y,
                                                    uint32_t z, uint32_t pattern_id) {
    return simcore::MultiblockController(id, x, y, z, pattern_id,
                                         std::vector<uint32_t>{});
}

struct Rig {
    entt::entity entity = entt::null;
    uint64_t controller_id = 0;
};

static Rig installReactor(Fixture& f, uint32_t x, uint32_t y, uint32_t z,
                          uint32_t pattern_id, EnergyType type, int32_t capacity,
                          int32_t stored, int slots) {
    Rig rig;
    rig.controller_id = f.controllers.size() + 1;
    f.controllers.emplace(rig.controller_id,
                          makeController(rig.controller_id, x, y, z, pattern_id));
    auto ent = f.reg.create();
    f.reg.emplace<simcore::Position>(ent, x, y, z);
    f.reg.emplace<simcore::MachineComponent>(ent, kLcrBlockId, rig.controller_id,
                                             x, y, z, rig.controller_id);
    f.reg.emplace<simcore::EnergyStorage>(ent, capacity, stored, 32, 0, 1, type);
    f.reg.emplace<simcore::RecipeProgress>(ent);
    simcore::InventoryContainer container(0, static_cast<uint16_t>(slots),
                                          std::vector<simcore::InventorySlot>{});
    container.slots.resize(slots);
    f.reg.emplace<simcore::InventoryContainer>(ent, container);
    rig.entity = ent;
    return rig;
}

static void addHatch(simcore::MultiblockController& ctrl, simcore::HatchType type,
                     bool present, uint32_t x, uint32_t y, uint32_t z,
                     uint16_t slot_start = 0, uint16_t slot_end = 0) {
    simcore::HatchSlot h;
    h.type = type;
    h.present = present;
    h.world_x = x; h.world_y = y; h.world_z = z;
    h.slot_start = slot_start;
    h.slot_end = slot_end;
    h.tier = 2;
    ctrl.hatches.push_back(h);
}

static void addItemHatches(simcore::MultiblockController& ctrl,
                           uint16_t in_start, uint16_t in_end,
                           uint16_t out_start, uint16_t out_end) {
    addHatch(ctrl, simcore::HatchType::ITEM_IN, true, 0, 0, 0, in_start, in_end);
    addHatch(ctrl, simcore::HatchType::ITEM_OUT, true, 0, 0, 0, out_start, out_end);
}

static void startRecipe(Fixture& f, entt::entity ent, const char* recipe_id,
                        int remaining_ticks) {
    auto& p = f.reg.get<simcore::RecipeProgress>(ent);
    p.recipe_id = recipe_id;
    p.remaining_ticks = static_cast<uint32_t>(remaining_ticks);
    p.is_processing = true;
}

// ---------------------------------------------------------------------------
// Controller selection
// ---------------------------------------------------------------------------

static void test_LCRSystem_zero_controller_id_is_skipped() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 10, 70, 10, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));
    f->controllers[rig.controller_id].id = 0;  // LCRSystem.cpp:40

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "id == 0 -> controller skipped, no EU spent");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "no update published");
}

static void test_LCRSystem_pattern_id_is_not_checked() {
    // The tick only requires that patterns_.getPattern(3) exists
    // (LCRSystem.cpp:41-42). Controllers with pattern ids 1, 2, 4, 99 are all
    // ticked as LCRs — contrast EBFSystem.cpp:44, which whitelists {1, 4}.
    for (uint32_t pattern : {0u, 1u, 2u, 4u, 99u}) {
        auto f = makeFixture();
        auto rig = installReactor(*f, 11, 70, 11, pattern, EnergyType::ELECTRICITY,
                                  10000, 500, 2);
        startRecipe(*f, rig.entity, "test_lcr_bronze_plate",
                    static_cast<int>(kRecipeDuration));
        f->sys->tick(0.05f);
        CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                     "any pattern_id is ticked as an LCR");
    }
}

static void test_LCRSystem_controller_without_matching_entity_is_a_noop() {
    auto f = makeFixture();
    const uint64_t id = 77;
    f->controllers.emplace(id, makeController(id, 400, 300, 400, 3));
    f->sys->tick(0.05f);
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "no entity -> nothing published");
}

static void test_LCRSystem_empty_controller_map_is_a_noop() {
    auto f = makeFixture();
    f->sys->tick(0.05f);
    f->sys->tick(1.0f);
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "no controllers -> no publishes");
}

static void test_LCRSystem_dt_is_ignored() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 12, 70, 12, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);
    const int32_t after_small = f->reg.get<simcore::EnergyStorage>(rig.entity).current;
    const uint32_t ticks_small = f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks;

    f->sys->tick(100.0f);
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 after_small - static_cast<int32_t>(kLegacyEuPerTick),
                 "one tick costs one eu, whatever dt is");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 ticks_small - 1, "one tick advances one progress tick");
}

static void test_LCRSystem_two_controllers_are_both_ticked() {
    auto f = makeFixture();
    auto a = installReactor(*f, 13, 70, 13, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    auto b = installReactor(*f, 14, 70, 14, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(*f, a.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));
    startRecipe(*f, b.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(a.entity).current,
                 500 - static_cast<int32_t>(kLegacyEuPerTick), "reactor A charged");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(b.entity).current,
                 500 - static_cast<int32_t>(kLegacyEuPerTick), "reactor B charged");
    CHECK_EQ_INT(f->events->updates.size(), size_t(2), "each reactor publishes its own update");
}

// ---------------------------------------------------------------------------
// ENERGY hatch detection and the absent-hatch FALLBACK
// ---------------------------------------------------------------------------

static void test_LCRSystem_present_energy_hatch_does_not_block_execution() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 20, 70, 20, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addHatch(f->controllers[rig.controller_id], simcore::HatchType::ENERGY, true,
             20, 71, 20);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 500 - static_cast<int32_t>(kLegacyEuPerTick),
                 "a present ENERGY hatch runs the recipe normally");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1), "and publishes");
}

static void test_LCRSystem_absent_energy_hatch_falls_back_to_the_controller() {
    // The openspec task asked for "absent hatch rejection/fallback". The code
    // FALLS BACK (LCRSystem.cpp:56-64): with no ENERGY hatch the endpoint is
    // the controller's own position, and the recipe runs. The rejection half of
    // the question belongs to EBFSystem, which returns early instead.
    auto f = makeFixture();
    auto rig = installReactor(*f, 21, 70, 21, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    // Only a non-ENERGY hatch, and one ENERGY hatch that is NOT present.
    addHatch(f->controllers[rig.controller_id], simcore::HatchType::FLUID_IN, true,
             21, 71, 21);
    addHatch(f->controllers[rig.controller_id], simcore::HatchType::ENERGY, false,
             99, 99, 99);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 500 - static_cast<int32_t>(kLegacyEuPerTick),
                 "an absent ENERGY hatch is NOT a rejection: the recipe runs");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                 "and the block-entity update is still published");
}

static void test_LCRSystem_first_present_energy_hatch_wins() {
    // The scan breaks on the FIRST HatchType::ENERGY with present == true
    // (LCRSystem.cpp:50-55), so a second, higher-tier hatch is ignored. With
    // no PipeEnergyClient the endpoint coordinates are not observable through
    // this system, so the pinned property is that the first present ENERGY
    // hatch is the one that makes the reactor run at all — a list of only
    // ABSENT ENERGY hatches is indistinguishable from an empty list.
    auto f = makeFixture();
    auto rig = installReactor(*f, 22, 70, 22, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addHatch(f->controllers[rig.controller_id], simcore::HatchType::ENERGY, false, 1, 1, 1);
    addHatch(f->controllers[rig.controller_id], simcore::HatchType::ENERGY, true, 2, 2, 2);
    addHatch(f->controllers[rig.controller_id], simcore::HatchType::ENERGY, true, 3, 3, 3);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                 "the first PRESENT energy hatch is selected, the rest are skipped");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 500 - static_cast<int32_t>(kLegacyEuPerTick), "and the recipe is charged");
}

static void test_LCRSystem_hatch_tier_is_carried_in_the_hatch_payload() {
    // The hatch list is published verbatim through buildHatchUpdateData(), so
    // the tier recorded on the HatchSlot reaches the client. Pinned through
    // the published hatch count plus the tier of the slot we installed.
    auto f = makeFixture();
    auto rig = installReactor(*f, 23, 70, 23, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addHatch(f->controllers[rig.controller_id], simcore::HatchType::ENERGY, true,
             23, 71, 23);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->events->updates.size(), size_t(1), "one update published");
    CHECK_EQ_INT(f->events->updates[0].hatch_count, 1,
                 "the hatch list is published alongside the update");
    CHECK_EQ_INT(f->controllers[rig.controller_id].hatches[0].tier, 2,
                 "the installed hatch carries tier 2 into buildHatchUpdateData");
}

// ---------------------------------------------------------------------------
// Energy cost: resourceAmountPerTick vs energy_cost
// ---------------------------------------------------------------------------

static void test_LCRSystem_resource_requirements_drive_the_per_tick_cost() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 30, 70, 30, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 500 - static_cast<int32_t>(kResourcePerTick),
                 "resourceAmountPerTick() (32) is the per-tick cost, not energy_cost");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1, "one tick advanced");
}

static void test_LCRSystem_legacy_eu_drives_the_per_tick_cost() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 31, 70, 31, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 500 - static_cast<int32_t>(kLegacyEuPerTick),
                 "a recipe with no resource_requirements uses energy_cost (7.0)");
}

static void test_LCRSystem_stalls_when_the_buffer_is_below_the_cost() {
    auto f = makeFixture();
    // 31 EU against a 32 EU/tick requirement.
    auto rig = installReactor(*f, 32, 70, 32, 3, EnergyType::ELECTRICITY, 10000, 31, 2);
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 31,
                 "a starved LCR spends nothing");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration, "and makes no progress");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0),
                 "the stall returns before the publish (LCRSystem.cpp:143)");
}

static void test_LCRSystem_stall_boundary_is_inclusive() {
    // current < cost stalls; current == cost runs and lands on zero.
    auto f = makeFixture();
    auto rig = installReactor(*f, 33, 70, 33, 3, EnergyType::ELECTRICITY, 10000,
                              static_cast<int32_t>(kResourcePerTick), 2);
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 0,
                 "current == cost is not a stall: the tick runs and lands on zero");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1, "and advances one tick");
}

static void test_LCRSystem_full_run_consumes_exact_energy_and_produces_output() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 34, 70, 34, 3, EnergyType::ELECTRICITY, 1000, 500, 2);
    auto& ctrl = f->controllers[rig.controller_id];
    addItemHatches(ctrl, 0, 1, 1, 2);
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));

    // The decrement and the completion branch live in the SAME tick
    // (LCRSystem.cpp:146 then :163), so exactly `duration` charged ticks finish
    // the recipe: 4 ticks take it 4 -> 3 -> 2 -> 1 -> 0 and the last one pays out.
    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const int32_t total = static_cast<int32_t>(kRecipeDuration) *
                          static_cast<int32_t>(kResourcePerTick);
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500 - total,
                 "duration ticks x 32 EU/tick is the exact total charge");

    const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
    CHECK_EQ_INT(p.remaining_ticks, 0u, "the recipe ran to completion");
    CHECK(!p.is_processing, "is_processing is false at completion");
    CHECK(p.needs_output, "needs_output is set at completion");
    CHECK(p.recipe_id.empty(), "recipe_id is cleared at completion");

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[1].item_id, kBronzePlate,
                 "the product lands in the ITEM_OUT slot");
    CHECK_EQ_INT(inv.slots[1].count, 1, "exactly one product");

    // A fifth tick must NOT pay out a second time: recipe_id is empty, so the
    // system re-enters the input-matching branch and finds nothing.
    f->sys->tick(0.05f);
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500 - total,
                 "no EU is charged once the recipe is done");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[1].count, 1,
                 "and the product is not duplicated");
}

static void test_LCRSystem_completion_publishes_capacity() {
    // FINDING E (the healthy contrast to gp-bbbl): the LCR publishes
    // energy.capacity; RotareGeneratorSystem does not.
    auto f = makeFixture();
    auto rig = installReactor(*f, 35, 70, 35, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const auto& last = f->events->updates.back();
    CHECK_EQ_INT(last.energy_capacity, 10000u,
                 "energy_capacity is the real capacity, not 0 (contrast gp-bbbl)");
    CHECK_EQ_INT(last.energy_type, EnergyType::ELECTRICITY,
                 "the energy type is published as ELECTRICITY");
    CHECK_EQ_INT(last.heat_ratio, 0.0f, "the LCR hardcodes heat_ratio to 0.0f");
    CHECK_EQ_INT(last.slots_in, 1, "slots_in is the ITEM_IN slot END");
    CHECK_EQ_INT(last.machine_id, kLcrBlockId, "the block id is the LCR controller");
}

static void test_LCRSystem_progress_series_runs_to_one() {
    // FINDING D: pct = 1 - remaining/duration, so a 4-tick recipe publishes
    // 0.25, 0.5, 0.75 and then the completion tick publishes nothing more
    // (recipe_id is cleared, so the pct block is skipped).
    auto f = makeFixture();
    auto rig = installReactor(*f, 36, 70, 36, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    // OBSERVED, not assumed. Two facts combine:
    //   1. the decrement at LCRSystem.cpp:147 happens BEFORE the publish block
    //      at :236, so a tick that advances publishes 1-(n-1)/duration rather
    //      than 1-n/duration — the series starts at 0.25, not 0.0;
    //   2. the completion branch clears recipe_id at :181, which the publish
    //      block reads at :236, so the tick that finishes the recipe reports
    //      progress 0.0 rather than 1.0.
    // Net effect: the series is 0.25, 0.5, 0.75, 0.0 — it never reaches 1.0, and
    // it RESETS TO 0.0 on the tick a client needs most.
    CHECK_EQ_INT(f->events->updates.size(), size_t(kRecipeDuration),
                 "one publish per tick, including the completion tick");
    const float series[] = {0.25f, 0.5f, 0.75f, 0.0f};
    for (size_t i = 0; i < f->events->updates.size() && i < 4; ++i) {
        CHECK(fabs(f->events->updates[i].progress - series[i]) < 1e-5f,
              "the published series is 0.25, 0.5, 0.75, 0.0");
    }
    CHECK(fabs(f->events->updates.back().progress - 0.0f) < 1e-5f,
          "the completion tick publishes 0.0: recipe_id is cleared before the read");
    CHECK(fabs(f->events->updates.back().progress - 1.0f) > 1e-5f,
          "progress 1.0 is unreachable through this publish path");

    // A client polling this stream sees progress jump from 0.75 back to 0.0
    // while the product appears in the inventory — the bar never fills.
    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[1].item_id, kLegacyOutput,
                 "the product is in the output slot on the same 0.0 tick");
}

static void test_LCRSystem_missing_recipe_clears_progress() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 37, 70, 37, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(*f, rig.entity, "no_such_recipe", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
    CHECK(p.recipe_id.empty(), "a vanished recipe id is cleared");
    CHECK(!p.is_processing, "is_processing is cleared with it");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "a vanished recipe costs nothing");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "the early return skips the publish");
}

// ---------------------------------------------------------------------------
// Finding A: EU-only requirements are not orchestrated
// ---------------------------------------------------------------------------

static void test_LCRSystem_eu_only_requirements_are_not_orchestrated() {
    // FINDING A (LCRSystem.cpp:117-123): `orchestrated` requires
    // has_non_eu_requirement, i.e. at least one requirement whose kind is NOT
    // EU. Every shipped chemical_reactor.yaml recipe declares EU-only
    // requirements, so the sentinel is false. Without a CraftReservationClient
    // the code then falls to the pipeClient_ branch, and with no pipe client
    // either the tick simply returns — which is why this suite always passes a
    // null pipe client and asserts the legacy debit instead.
    auto f = makeFixture();
    const auto* recipe = f->recipes->getRecipeById("test_lcr_bronze_plate");
    CHECK(recipe != nullptr, "the EU-only recipe is loaded");
    CHECK(recipe->hasResourceRequirements(), "it declares resource_requirements");
    const bool has_non_eu = std::any_of(
        recipe->resource_requirements.begin(), recipe->resource_requirements.end(),
        [](const auto& r) { return r.kind != gtnh::common::ResourceKind::EU; });
    CHECK(!has_non_eu, "an EU-only requirement set has no non-EU entry");
    CHECK_EQ_INT(recipe->resourceAmountPerTick(), kResourcePerTick,
                 "resourceAmountPerTick sums the EU requirement to 32");

    // The legacy `eu:` field is untouched by the requirements block, and it is
    // what a recipe WITHOUT requirements would be charged.
    CHECK(fabs(recipe->energy_cost) < 1e-6f,
          "the EU-only recipe carries no `eu:` value, so its energy_cost is 0");
}

static void test_LCRSystem_zero_cost_recipe_runs_for_free() {
    // The consequence of finding A: a recipe with EU-only requirements and no
    // `eu:` field is charged resourceAmountPerTick() here, but a recipe with
    // NEITHER requirements NOR `eu:` would debit 0. Pinned with a third recipe
    // shape loaded inline so the zero-cost path is actually exercised.
    auto f = makeFixture();
    const char* path_env = getenv("TMPDIR");
    std::string dir = path_env && *path_env ? std::string(path_env) + "/" : "./";
    const std::string path = dir + "lcr_zero_cost_recipe.yaml";
    FILE* fh = fopen(path.c_str(), "w");
    CHECK(fh != nullptr, "the scratch recipe file can be written");
    if (fh) {
        fputs("class: chemical_reactor\n"
              "recipes:\n"
              "  - name: test_lcr_zero_cost\n"
              "    inputs:\n"
              "      - { item: \"0:1110:001:71\", count: 1 }\n"
              "    outputs:\n"
              "      - { item: \"0:1110:001:72\", count: 1 }\n"
              "    duration: 4\n"
              "    min_tier: 0\n"
              "    max_tier: 32767\n",
              fh);
        fclose(fh);
        Fixture::tempFiles().push_back(path);
    }

    auto rig = installReactor(*f, 38, 70, 38, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);
    f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0] =
        {ItemId::pack("0:1110:001:71"), 1, 0};

    if (fh && f->recipes->loadRecipesFromYamlFile(path)) {
        f->sys->tick(0.05f);
        const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
        CHECK_EQ_INT(p.recipe_id, std::string("test_lcr_zero_cost"),
                     "the zero-cost recipe matched and started");
        CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                     "and consumed no EU at all");
        CHECK_EQ_INT(p.remaining_ticks, kRecipeDuration, "the recipe is running");
    } else {
        CHECK(false, "the zero-cost recipe file loads");
    }
}

// ---------------------------------------------------------------------------
// onConsumeResponse
// ---------------------------------------------------------------------------

static void test_LCRSystem_on_consume_response_unknown_node_returns_false() {
    // FINDING B: `consumed` is only consulted AFTER the pending-lookup. The
    // lookup misses, so EVERY value returns false regardless of `consumed`
    // (LCRSystem.cpp:306-309). The `consumed <= 0 -> true` branch is
    // unreachable for an uncorrelated node.
    auto f = makeFixture();
    CHECK(!f->sys->onConsumeResponse(12345, 32, 0),
          "a node with no pending consume returns false");
    CHECK(!f->sys->onConsumeResponse(12345, 0, 0),
          "a zero consumed on an uncorrelated node also returns false");
    CHECK(!f->sys->onConsumeResponse(12345, -1, 0),
          "a negative consumed on an uncorrelated node also returns false");
    CHECK(!f->sys->onConsumeResponse(0, 0, 0),
          "node_id 0 is treated the same as any other uncorrelated id");
}

static void test_LCRSystem_on_consume_response_credits_the_buffer() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 39, 70, 39, 3, EnergyType::ELECTRICITY, 10000, 100, 2);
    const uint64_t node_id = static_cast<uint64_t>(rig.entity);

    // FINDING B: the pending entry is only written on the `pipeClient_` stall
    // branch (LCRSystem.cpp:137-141). This fixture passes a NULL pipe client,
    // so the stall records nothing and NO response can ever be correlated — the
    // energy credit path (LCRSystem.cpp:315) is dead under a null client.
    // Pinned as observed, not "fixed" in src/.
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));
    f->reg.get<simcore::EnergyStorage>(rig.entity).current = 0;
    f->sys->tick(0.05f);  // 0 < 32 -> the stall branch

    CHECK(!f->sys->onConsumeResponse(node_id, 32, 0),
          "with a null pipe client the stall records no pending consume, so the "
          "response is uncorrelated and no EU is credited");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 0,
                 "the energy-credit path is unreachable without a pipe client");
}

// ---------------------------------------------------------------------------
// Slot ranges and recipe start
// ---------------------------------------------------------------------------

static void test_LCRSystem_registry_supplies_the_output_range() {
    auto f = makeFixture();
    installRegistry(kLcrBlockId, 1, 1);  // after makeFixture, which resets it
    auto rig = installReactor(*f, 40, 70, 40, 3, EnergyType::ELECTRICITY, 1000, 500, 2);
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[1].item_id, kBronzePlate,
                 "with a registry the product lands in the output slot");
}

static void test_LCRSystem_output_is_dropped_with_no_output_range() {
    // With no ITEM_OUT hatch and no registry entry, both placement loops
    // (LCRSystem.cpp:166-180) iterate an empty range, so the product is
    // destroyed rather than deposited — the same shape EBFSystem exhibits.
    auto f = makeFixture();
    auto rig = installReactor(*f, 41, 70, 41, 3, EnergyType::ELECTRICITY, 1000, 500, 2);
    startRecipe(*f, rig.entity, "test_lcr_bronze_plate", static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[0].item_id, 0, "slot 0 is untouched");
    CHECK_EQ_INT(inv.slots[1].item_id, 0, "slot 1 is untouched: the product is dropped");
    CHECK(f->reg.get<simcore::RecipeProgress>(rig.entity).needs_output,
          "the recipe still completes and flags needs_output");
}

static void test_LCRSystem_matching_inputs_start_the_recipe() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 42, 70, 42, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);
    f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0] = {kBronzeIngot, 2, 0};

    f->sys->tick(0.05f);

    const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
    CHECK_EQ_INT(p.recipe_id, std::string("test_lcr_bronze_plate"), "inputs matched");
    CHECK(p.is_processing, "the recipe is running");
    CHECK_EQ_INT(p.remaining_ticks, kRecipeDuration, "remaining_ticks is the duration");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].count, 1,
                 "one bronze_ingot is consumed, one remains");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "the start tick itself does not charge EU");
}

static void test_LCRSystem_no_matching_inputs_leave_the_reactor_idle() {
    auto f = makeFixture();
    auto rig = installReactor(*f, 43, 70, 43, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);

    f->sys->tick(0.05f);

    CHECK(f->reg.get<simcore::RecipeProgress>(rig.entity).recipe_id.empty(),
          "an empty inventory starts nothing");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                 "an idle reactor still publishes its state once per tick");
}

static void test_LCRSystem_tier_comes_from_the_registration_not_the_buffer() {
    // FINDING C: the tier gate lives in findRecipeByInputs(), which reads
    // getMachineTier(machine_id) — the tier the RecipeManager was told at
    // registerMachineClass() time (main.cpp:233 passes 0). EnergyStorage::tier
    // is never consulted by LCRSystem, so changing it must not change which
    // recipes match.
    auto f = makeFixture();
    CHECK_EQ_INT(f->recipes->getMachineTier(kLcrBlockId), 0,
                 "the LCR controller is registered at tier 0");

    const auto* recipe = f->recipes->getRecipeById("test_lcr_bronze_plate");
    CHECK(recipe != nullptr, "the recipe is loaded");
    CHECK(recipe->min_tier <= 0 && 0 <= recipe->max_tier,
          "a 0..32767 recipe is visible to a tier-0 machine");

    // Raising the EnergyStorage tier changes nothing about the match.
    auto rig = installReactor(*f, 44, 70, 44, 3, EnergyType::ELECTRICITY, 10000, 500, 2);
    f->reg.get<simcore::EnergyStorage>(rig.entity).tier = 4;
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);
    f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0] = {kBronzeIngot, 1, 0};

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).recipe_id,
                 std::string("test_lcr_bronze_plate"),
                 "a tier-4 buffer still matches the tier-0 registration");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration,
                 "the start sets remaining_ticks to the duration, not duration-1");
    // Now tick again: the recipe is active and the 32 EU is charged.
    f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks = kRecipeDuration;
    f->reg.get<simcore::RecipeProgress>(rig.entity).is_processing = true;
    f->sys->tick(0.05f);
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 500 - static_cast<int32_t>(kResourcePerTick),
                 "the active tick is charged 32 EU regardless of the buffer tier");
}

static void test_LCRSystem_energy_type_is_never_checked() {
    // Unlike EBFSystem, which rejects a wrong energy type
    // (EBFSystem.cpp:192-193), LCRSystem has no energy-type gate at all: the
    // LCR recipe's own `energy_in` filter is applied in findRecipeByInputs()
    // against the REGISTERED energy_in, never against EnergyStorage::type. A
    // HEAT-typed buffer runs an ELECTRICITY LCR recipe without complaint.
    auto f = makeFixture();
    auto rig = installReactor(*f, 45, 70, 45, 3, EnergyType::HEAT, 10000, 500, 2);
    startRecipe(*f, rig.entity, "test_lcr_legacy", static_cast<int>(kRecipeDuration));

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 500 - static_cast<int32_t>(kLegacyEuPerTick),
                 "a HEAT buffer is charged exactly like an ELECTRICITY one");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1), "and the update is published");
    CHECK_EQ_INT(f->events->updates[0].energy_type, EnergyType::HEAT,
                 "the published energy type is the buffer's, not the recipe's");
}



#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== lcr_system test suite ===\n\n");

    // Controller selection
    TEST(LCRSystem_zero_controller_id_is_skipped);
    TEST(LCRSystem_pattern_id_is_not_checked);
    TEST(LCRSystem_controller_without_matching_entity_is_a_noop);
    TEST(LCRSystem_empty_controller_map_is_a_noop);
    TEST(LCRSystem_dt_is_ignored);
    TEST(LCRSystem_two_controllers_are_both_ticked);

    // ENERGY hatch detection and the absent-hatch fallback
    TEST(LCRSystem_present_energy_hatch_does_not_block_execution);
    TEST(LCRSystem_absent_energy_hatch_falls_back_to_the_controller);
    TEST(LCRSystem_first_present_energy_hatch_wins);
    TEST(LCRSystem_hatch_tier_is_carried_in_the_hatch_payload);

    // Energy cost
    TEST(LCRSystem_resource_requirements_drive_the_per_tick_cost);
    TEST(LCRSystem_legacy_eu_drives_the_per_tick_cost);
    TEST(LCRSystem_stalls_when_the_buffer_is_below_the_cost);
    TEST(LCRSystem_stall_boundary_is_inclusive);
    TEST(LCRSystem_full_run_consumes_exact_energy_and_produces_output);
    TEST(LCRSystem_completion_publishes_capacity);
    TEST(LCRSystem_progress_series_runs_to_one);
    TEST(LCRSystem_missing_recipe_clears_progress);

    // Finding A: EU-only requirements are not orchestrated
    TEST(LCRSystem_eu_only_requirements_are_not_orchestrated);
    TEST(LCRSystem_zero_cost_recipe_runs_for_free);

    // onConsumeResponse
    TEST(LCRSystem_on_consume_response_unknown_node_returns_false);
    TEST(LCRSystem_on_consume_response_credits_the_buffer);

    // Slot ranges, recipe start, tier, energy type
    TEST(LCRSystem_registry_supplies_the_output_range);
    TEST(LCRSystem_output_is_dropped_with_no_output_range);
    TEST(LCRSystem_matching_inputs_start_the_recipe);
    TEST(LCRSystem_no_matching_inputs_leave_the_reactor_idle);
    TEST(LCRSystem_tier_comes_from_the_registration_not_the_buffer);
    TEST(LCRSystem_energy_type_is_never_checked);

    for (const auto& path : Fixture::tempFiles()) {
        remove(path.c_str());
    }
    Fixture::tempFiles().clear();
    MachineRegistry::setInstance(nullptr);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
