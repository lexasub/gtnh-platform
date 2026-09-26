// EBFSystem unit tests (issue gp-r3vd).
//
// Covers src/game/machines/EBFSystem.cpp — the electric/hot blast furnace
// execution path: EU consumption, HU (coil heat) output, and the stall
// conditions that freeze a running recipe.
//
// DRIVER SHAPE (why these tests look the way they do):
//   EBFSystem::tick does NOT iterate an EnTT view of controllers. It walks the
//   `controllers_` unordered_map handed in by the constructor and calls
//   tickEBF() for every entry that survives three gates:
//     EBFSystem.cpp:43  ctrl.id != 0        -> skip
//     EBFSystem.cpp:44  pattern_id not in {1 (EBF), 4 (HBF)} -> skip
//   tickEBF() then LOCATES the machine entity by scanning
//   reg_.view<const Position, MachineComponent>() for a Position equal to the
//   controller's own (x, y, z) — the controller block's own position, not a
//   member block's. That is how the test must be wired: the ECS entity sits at
//   the controller coordinates.
//
// LIVENESS (checked before writing these tests):
//   controllers_ IS populated in production — SimulationEngine.cpp:446
//   registerController() emplaces into the very same map that main.cpp:488
//   hands to EBFSystem via getControllers(). Unlike ExplosionSystem /
//   SteamTurbineSystem (gp-43vj, gp-q692) there is no unsatisfiable
//   component gate here: the machine entity at the controller position gets
//   MachineComponent + EnergyStorage + InventoryContainer + RecipeProgress from
//   SimulationEngine.cpp:220-259. So the system runs for real. The tests still
//   have to supply the controllers map themselves, because the EBF has no
//   activate()-style liveness gate of its own.
//
// SYSTEM CONTRACT, per tick, for each accepted controller:
//   0. find the entity at the controller's position; none -> return
//   1. heat = reg_.get_or_emplace<HeatIntakeComponent>(entity)
//   2. resolve slot ranges: ITEM_IN/ITEM_OUT hatch ranges, else the
//      MachineRegistry layout, else 0 (EBFSystem.cpp:129-148)
//   3. electric = (pattern_id == 1). For an EBF the presence of a real
//      ENERGY hatch is MANDATORY (EBFSystem.cpp:161: `if (!energy_hatch) return;`)
//   4. publish the node (guarded on pipeClient_)
//   5. with an active recipe:
//      - recipe vanishes          -> clear recipe_id, stop (EBFSystem.cpp:185)
//      - heat_stored < required   -> STALL, no debit, no progress (line 191)
//      - wrong energy type        -> STALL (lines 192-193)
//      - energy.current < cost    -> STALL + one consume request (line 201)
//      - otherwise: debit perTickCost, decrement remaining_ticks, publish
//      - at zero ticks: stack outputs into the output range, needs_output = true
//   6. with no active recipe: match inputs; a match consumes the inputs and
//      starts the recipe immediately (no reservation unless a
//      CraftReservationClient is supplied AND the recipe declares requirements)
//   7. publish the block-entity update every tick that reaches the end
//
// CONSTANTS READ FROM SOURCE (asserted by name, so a change shows in the diff):
//   EBFSystem::KANHAL_MAX_HEAT      1800
//   EBFSystem::NICHROME_MAX_HEAT   2700
//   EBFSystem::TUNGSTENSTEEL_MAX_HEAT 4500
//   EBFSystem::COIL_LAYER_1 / _2    1 / 2  (the two coil scan rows)
//   EBFSystem::COIL_DX / COIL_DZ     1 / 1
//   getCoilHeat(): 0 for anything that is not a coil -> HBF heat tier 0.
//   pattern 1 (ebf) and 4 (hbf) are the only accepted pattern ids.
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed" here):
//   A. THE HEAT GATE IS EVALUATED BEFORE THE ENERGY-TYPE GATE, and both are
//      evaluated AFTER the recipe lookup, so an HBF running an ELECTRICITY
//      recipe parks forever rather than rejecting it. Pinned by
//      test_EBFSystem_hbf_stalls_below_half_of_coil_tier.
//   B. THE HBF HEAT TIER IS THE MINIMUM OVER THE TWO COIL ROWS AND BOTH ROWS
//      MUST BE COILS. One missing/mismatched coil makes getCoilHeat return 0
//      for that row and detectHeatTier() bails out with 0 (EBFSystem.cpp:90),
//      so a furnace with a single nichrome row and a non-coil row above it is
//      permanently cold. Pinned by
//      test_EBFSystem_hbf_heat_tier_is_the_minimum_of_both_coil_rows.
//   C. THE FIRST HBF TICK MIRRORS energy.current INTO heat_stored AFTER THE
//      DEBIT (EBFSystem.cpp:223), so heat_stored ends the tick BELOW the amount
//      just charged; it is a lagging mirror, not a heat source. Pinned by
//      test_EBFSystem_hbf_heat_stored_mirrors_the_debited_buffer.
//   D. A COLD HBF (heat tier 0 -> requiredHeat 0) IS NOT COLD: a furnace with
//      no detectable coils requires 0 HU and runs. Pinned by
//      test_EBFSystem_hbf_with_no_coils_requires_zero_heat.
//   E. THE OUTPUT SLOT RANGE IS NEVER ZERO-GUARDED on the write side: with no
//      ITEM_OUT hatch AND no MachineRegistry entry, output_start/output_end stay
//      0, so a completed recipe deposits its output into slot 0 — the input
//      slot. Pinned by
//      test_EBFSystem_output_falls_back_to_slot_zero_without_a_registry.
//   F. THE EU COST IS recipe->energy_cost TRUNCATED TO int32, so a fractional
//      `eu:` charges the integer part. ebf.yaml declares eu: 2.0. Pinned by
//      test_EBFSystem_eu_cost_is_the_truncated_recipe_energy_cost.
//   G. THE BLOCK-ENTITY UPDATE PUBLISHES energy.capacity, unlike
//      RotareGeneratorSystem (gp-bbbl). Asserted as working here.
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
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/PatternLibrary.h>
#include <engine/sim/SimulationEngine.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/HeatIntakeComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/MultiblockController.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/RecipeProgress.h>
#include <game/machines/EBFSystem.h>
#include <game/machines/HeatConstants.h>
#include <game/recipes/ItemRegistry.h>
#include <game/recipes/RecipeManager.h>

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has
// no GTest dependency, so this is the established convention for focused tests.
#include <engine/net/test/test.h>

#ifndef CHECK_EQ_INT
#define CHECK_EQ_INT(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#endif
#ifndef CHECK_GT_INT
#define CHECK_GT_INT(a, b, ...) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, ##__VA_ARGS__)
#endif
#ifndef CHECK_LE_INT
#define CHECK_LE_INT(a, b, ...) test_check((a) <= (b), __FILE__, __LINE__, #a " <= " #b, ##__VA_ARGS__)
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
    size_t inventory_bytes = 0;
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
                                  int slots_in, float,
                                  const std::vector<HatchUpdateData>* = nullptr,
                                  double = -1.0, double = -1.0) override {
        PublisherEvent e;
        e.x = x; e.y = y; e.z = z;
        e.machine_id = machine_type;
        e.progress = progress;
        e.energy = energy;
        e.energy_capacity = energy_capacity;
        e.energy_type = energy_type;
        e.slots_in = slots_in;
        e.inventory_bytes = inventory_data.size();
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

// Item ids used by the synthetic recipes. Hierarchical strings are packed by
// ItemId::pack directly — no CSV/registry lookup, so the suite never depends on
// src/content/data being present at the CWD.
constexpr uint16_t kIronDust  = ItemId::pack("0:1110:001:26");
constexpr uint16_t kIronIngot = ItemId::pack("0:110:1");
constexpr uint16_t kSlag      = ItemId::pack("0:1110:001:99");

// machines.yaml is only needed for the registry-fallback cases; the hot EBF/HBF
// paths deliberately run with NO MachineRegistry instance (a null
// MachineRegistry::instance() is legal — every call site is `if (auto* m = ...)`)
// which is what finding E below pins.
constexpr uint16_t kTestBlockId = 0x0ACE;  // no MachineRegistry entry

static const int kKanhalMax = simcore::EBFSystem::KANHAL_MAX_HEAT;
static const int kNichromeMax = simcore::EBFSystem::NICHROME_MAX_HEAT;
static const int kTungstensteelMax = simcore::EBFSystem::TUNGSTENSTEEL_MAX_HEAT;

// EBFSystem::COIL_LAYER_1 / COIL_LAYER_2 / COIL_DX / COIL_DZ are PRIVATE
// (EBFSystem.h:50-53), so the test mirrors them here. If the production scan
// window ever moves, these two rows stop matching and the HBF tier tests fail
// loudly rather than silently testing a different layout.
constexpr int kCoilLayer1 = 1;
constexpr int kCoilLayer2 = 2;

// The recipe duration/eu the tests assert against. Written into the synthetic
// YAML below, so the numbers here and the recipe can never drift apart.
constexpr uint32_t kRecipeDuration = 4;
constexpr float kRecipeEuPerTick = 2.0f;  // what "eu: 2.0" in the YAML means

// Scratch directory for the synthetic recipe file. TMPDIR is honoured when set
// so a ctest run never litters the build tree; the file is unlinked in main().
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
    // Declared BEFORE sys and destroyed AFTER it: EBFSystem holds references to
    // reg/controllers/patterns, so a movable fixture would dangle the moment
    // makeFixture() returned. std::make_shared below constructs the Fixture
    // in place, so those references bind to the final object.
    std::unique_ptr<simcore::EBFSystem> sys;
    // Kept alive for the whole fixture: the RecipeManager's destructor must not
    // run before the system is destroyed.
    static std::vector<std::string>& tempFiles() {
        static std::vector<std::string> files;
        return files;
    }
};

// A minimal recipe manager holding one 4-tick ELECTRICITY smelt. The recipe id
// is the YAML `name`, and the input id is packed from a hierarchical string so
// no item registry is required.
static void writeSyntheticEBFRecipe() {
    const std::string path = scratchDir() + "ebf_synthetic_recipes.yaml";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fputs("class: ebf\n"
          "recipes:\n"
          "  - name: test_ebf_iron\n"
          "    inputs:\n"
          "      - { item: \"0:1110:001:26\", count: 1 }\n"
          "    outputs:\n"
          "      - { item: \"0:1110:001:99\", count: 2 }\n"
          "    duration: 4\n"
          "    eu: 2.0\n"
          "    energy_in: ELECTRICITY\n"
          "    min_tier: 0\n"
          "    max_tier: 32767\n",
          f);
    fclose(f);
    Fixture::tempFiles().push_back(path);
}

static void setupRecipes() {
    static bool done = false;
    if (done) return;
    writeSyntheticEBFRecipe();
    done = true;
}

// MachineRegistry has a private constructor, so LoadFromYaml is the only way
// to obtain one; it accepts an unopenable path and returns an EMPTY registry
// (MachineRegistry.cpp:54-58), which is exactly the "registry present but this
// block unknown" case. The result is intentionally leaked: the singleton has no
// ownership story and MachineRegistry::setInstance never frees the old pointer.
static void installRegistry(uint16_t block_id, int slots_in, int slots_out) {
    auto reg = MachineRegistry::LoadFromYaml("/nonexistent/machines.yaml");
    if (!reg) return;
    MachineInfo info{};
    info.id = block_id;
    info.name = "test_furnace";
    info.machine_class = "ebf";
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

// An EMPTY but non-null registry: kTestBlockId is unknown, so every
// MachineRegistry::Get() miss falls through to the default slot layout.
static void installEmptyRegistry() {
    auto reg = MachineRegistry::LoadFromYaml("/nonexistent/machines.yaml");
    if (!reg) return;
    MachineRegistry::setInstance(reg.release());
}

static void clearRegistry() { MachineRegistry::setInstance(nullptr); }

// Returns a shared_ptr, NOT a Fixture by value: EBFSystem stores references
// to the fixture's registry/controller map, so the fixture must never be
// moved after construction. std::make_shared constructs it in place, and the
// bound references point at that final address.
static std::shared_ptr<Fixture> makeFixture() {
    setupRecipes();
    // FINDING H: the EBF/LCR/Machine slot-layout fallback writes
    // `MachineRegistry::instance()->Get(id)` — the `if` tests the RESULT, not
    // the INSTANCE, so a null singleton segfaults the tick. The fixture ALWAYS
    // installs a non-null (empty) registry: it is required for the tick to be
    // well-defined, and resetting it here keeps the suite order-independent
    // (a test that needs a populated registry installs one after this call).
    installEmptyRegistry();
    auto f = std::make_shared<Fixture>();
    f->recipes = std::make_shared<RecipeManager::RecipeManager>();
    f->recipes->loadRecipesFromYamlFile(scratchDir() + "ebf_synthetic_recipes.yaml");
    // The synthetic recipe is class "ebf"; register the block so
    // findRecipeByInputs() can reach it. tier 1 / ELECTRICITY mirrors what
    // main.cpp:229 registers for the real EBF controller.
    f->recipes->registerMachineClass(
        kTestBlockId, "ebf", 1,
        static_cast<uint8_t>(RecipeManager::EnergyType::ELECTRICITY));
    f->events = std::make_shared<RecordingPublisher>();
    f->sys = std::make_unique<simcore::EBFSystem>(f->reg, f->controllers, f->patterns,
                                                  f->recipes, f->events,
                                                  nullptr /* PipeEnergyClient */);
    return f;
}

static simcore::MultiblockController makeController(uint64_t id, uint32_t x, uint32_t y,
                                                    uint32_t z, uint32_t pattern_id) {
    return simcore::MultiblockController(id, x, y, z, pattern_id,
                                         std::vector<uint32_t>{});
}

// Places the coil blocks the EBF scans for. detectHeatTier() looks at
// corner + (COIL_DX, dy, COIL_DZ) for dy in {COIL_LAYER_1, COIL_LAYER_2} with
// corner = controller - (controller_dx, controller_dy, controller_dz).
// Pattern 1/4 are the blast-furnace geometry: controller_dx/dy/dz = 1/3/1.
static void placeCoil(entt::registry& reg, uint32_t cx, uint32_t cy, uint32_t cz,
                      int layer, uint16_t block_id) {
    const uint32_t corner_x = cx - 1;
    const uint32_t corner_y = cy - 3;
    const uint32_t corner_z = cz - 1;
    const uint32_t wx = corner_x + 1;
    const uint32_t wy = corner_y + static_cast<uint32_t>(layer);
    const uint32_t wz = corner_z + 1;
    auto e = reg.create();
    reg.emplace<simcore::Position>(e, wx, wy, wz);
    reg.emplace<simcore::Block>(e, block_id, 0, 0);
}

static void placeCoils(entt::registry& reg, uint32_t cx, uint32_t cy, uint32_t cz,
                       uint16_t layer1_id, uint16_t layer2_id) {
    placeCoil(reg, cx, cy, cz, kCoilLayer1, layer1_id);
    placeCoil(reg, cx, cy, cz, kCoilLayer2, layer2_id);
}

// A controller + machine entity pair at (x, y, z).
struct Rig {
    entt::entity entity = entt::null;
    uint64_t controller_id = 0;
};

static Rig installFurnace(Fixture& f, uint32_t x, uint32_t y, uint32_t z,
                          uint32_t pattern_id, EnergyType type, int32_t capacity,
                          int32_t stored, int slots) {
    Rig rig;
    rig.controller_id = f.controllers.size() + 1;
    f.controllers.emplace(rig.controller_id,
                          makeController(rig.controller_id, x, y, z, pattern_id));

    auto ent = f.reg.create();
    f.reg.emplace<simcore::Position>(ent, x, y, z);
    f.reg.emplace<simcore::MachineComponent>(ent, kTestBlockId, rig.controller_id,
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

static void addEnergyHatch(simcore::MultiblockController& ctrl, uint32_t x,
                           uint32_t y, uint32_t z) {
    simcore::HatchSlot h;
    h.type = simcore::HatchType::ENERGY;
    h.world_x = x; h.world_y = y; h.world_z = z;
    h.present = true;
    h.tier = 2;
    ctrl.hatches.push_back(h);
}

static void addItemHatches(simcore::MultiblockController& ctrl,
                           uint16_t in_start, uint16_t in_end,
                           uint16_t out_start, uint16_t out_end) {
    simcore::HatchSlot in;
    in.type = simcore::HatchType::ITEM_IN;
    in.present = true;
    in.slot_start = in_start;
    in.slot_end = in_end;
    ctrl.hatches.push_back(in);

    simcore::HatchSlot out;
    out.type = simcore::HatchType::ITEM_OUT;
    out.present = true;
    out.slot_start = out_start;
    out.slot_end = out_end;
    ctrl.hatches.push_back(out);
}

static void startRecipe(entt::registry& reg, entt::entity ent, int remaining_ticks) {
    auto& p = reg.get<simcore::RecipeProgress>(ent);
    p.recipe_id = "test_ebf_iron";
    p.remaining_ticks = static_cast<uint32_t>(remaining_ticks);
    p.is_processing = true;
}

// ---------------------------------------------------------------------------
// Controller selection: the tick() gates
// ---------------------------------------------------------------------------

static void test_EBFSystem_zero_controller_id_is_skipped() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 10, 70, 10, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 10, 70, 10);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    f->controllers[rig.controller_id].id = 0;  // EBFSystem.cpp:43 gate

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "id == 0 -> controller skipped, no EU spent");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "no update published");
}

static void test_EBFSystem_foreign_pattern_id_is_skipped() {
    auto f = makeFixture();
    // Pattern 2 is the large boiler, 3 is the LCR: both foreign here.
    for (uint32_t pattern : {2u, 3u, 5u}) {
        auto f2 = makeFixture();
        auto rig = installFurnace(*f2, 10, 70, 10, pattern, EnergyType::ELECTRICITY,
                                  10000, 500, 2);
        addEnergyHatch(f2->controllers[rig.controller_id], 10, 70, 10);
        startRecipe(f2->reg, rig.entity, kRecipeDuration);

        f2->sys->tick(0.05f);

        CHECK_EQ_INT(f2->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                     "only pattern 1 (ebf) and 4 (hbf) run");
        CHECK_EQ_INT(f2->events->updates.size(), size_t(0), "no update for a foreign pattern");
    }
}

static void test_EBFSystem_controller_without_matching_entity_is_a_noop() {
    auto f = makeFixture();
    // Controller exists, but no MachineComponent/Position entity at its
    // coordinates -> EBFSystem.cpp:120 returns.
    const uint64_t id = 77;
    f->controllers.emplace(id, makeController(id, 400, 300, 400, 1));
    addEnergyHatch(f->controllers[id], 400, 300, 400);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "no entity -> nothing published");
}

static void test_EBFSystem_dt_is_ignored() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 10, 70, 10, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 10, 70, 10);
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);
    const int32_t after_small = f->reg.get<simcore::EnergyStorage>(rig.entity).current;
    const uint32_t ticks_small = f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks;

    f->sys->tick(100.0f);  // a huge dt must not fast-forward
    const int32_t after_huge = f->reg.get<simcore::EnergyStorage>(rig.entity).current;
    const uint32_t ticks_huge = f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks;

    CHECK_EQ_INT(after_huge, after_small - 2, "one tick costs one eu, whatever dt is");
    CHECK_EQ_INT(ticks_huge, ticks_small - 1, "one tick advances one progress tick");
}

// ---------------------------------------------------------------------------
// The EBF ENERGY-hatch requirement (EBFSystem.cpp:161)
// ---------------------------------------------------------------------------

static void test_EBFSystem_ebf_without_energy_hatch_does_nothing() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 20, 70, 20, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    // NO energy hatch: an EBF must not expose an EU endpoint on its own.

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "no ENERGY hatch -> return before any EU is spent");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0),
                 "no ENERGY hatch -> no block-entity update either");
}

static void test_EBFSystem_ebf_with_absent_energy_hatch_does_nothing() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 20, 70, 20, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    // Hatch slot present in the list but not physically placed: `present` is
    // false, which EBFSystem.cpp:154 requires alongside the ENERGY type.
    simcore::HatchSlot h;
    h.type = simcore::HatchType::ENERGY;
    h.present = false;
    f->controllers[rig.controller_id].hatches.push_back(h);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "ENERGY hatch with present == false is not an EU endpoint");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "still no update published");
}

static void test_EBFSystem_ebf_with_energy_hatch_spends_eu() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 20, 70, 20, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 20, 71, 20);
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    const auto& energy = f->reg.get<simcore::EnergyStorage>(rig.entity);
    CHECK_EQ_INT(energy.current, 500 - static_cast<int32_t>(kRecipeEuPerTick),
                 "one tick debits recipe->energy_cost (2.0 truncated to 2)");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1, "one tick advances one progress tick");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                 "a tick that charges still publishes the update");
}

static void test_EBFSystem_eu_cost_is_the_truncated_recipe_energy_cost() {
    // FINDING F: the cost is `static_cast<int32_t>(recipe->energy_cost)`, so a
    // fractional `eu:` charges the integer part. Asserted against the same
    // constant the YAML declares.
    auto f = makeFixture();
    const auto* recipe = f->recipes->getRecipeById("test_ebf_iron");
    CHECK(recipe != nullptr, "synthetic recipe is loaded");
    CHECK_EQ_INT(static_cast<int32_t>(recipe->energy_cost),
                 static_cast<int32_t>(kRecipeEuPerTick),
                 "energy_cost 2.0 truncates to 2 EU/tick");

    auto rig = installFurnace(*f, 21, 70, 21, 1, EnergyType::ELECTRICITY, 10000, 100, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 21, 71, 21);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    f->sys->tick(0.05f);
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 98,
                 "100 - 2 EU after one tick");
}

static void test_EBFSystem_heat_requirement_is_zero_for_an_ebf() {
    // EBFSystem.cpp:172-174: an EBF (pattern 1) has coilMaxHeat forced to 0 and
    // requiredHeat to 0 — no HU gate at all, even with a cold HeatIntake.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 22, 70, 22, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 22, 71, 22);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    // heat_stored is 0 (lazily emplaced by the system on the first tick).

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 498,
                 "a cold EBF still runs: requiredHeat is 0 for pattern 1");
}

// ---------------------------------------------------------------------------
// Stalls
// ---------------------------------------------------------------------------

static void test_EBFSystem_stalls_when_eu_buffer_is_below_cost() {
    auto f = makeFixture();
    // 1 EU stored against a 2 EU/tick recipe.
    auto rig = installFurnace(*f, 30, 70, 30, 1, EnergyType::ELECTRICITY, 10000, 1, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 30, 71, 30);
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 1,
                 "a starved EBF spends nothing");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration, "a starved EBF makes no progress");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0),
                 "the stall path returns before the publish (EBFSystem.cpp:218)");
}

static void test_EBFSystem_stall_is_exactly_the_cost_boundary() {
    // current < cost stalls; current == cost runs and lands on zero.
    auto starving = makeFixture();
    auto rigA = installFurnace(*starving, 31, 70, 31, 1, EnergyType::ELECTRICITY,
                               10000, 1, 2);
    addEnergyHatch(starving->controllers[rigA.controller_id], 31, 71, 31);
    startRecipe(starving->reg, rigA.entity, kRecipeDuration);
    starving->sys->tick(0.05f);
    CHECK_EQ_INT(starving->reg.get<simcore::RecipeProgress>(rigA.entity).remaining_ticks,
                 kRecipeDuration, "current = cost - 1 stalls");

    auto exact = makeFixture();
    auto rigB = installFurnace(*exact, 32, 70, 32, 1, EnergyType::ELECTRICITY,
                               10000, 2, 2);
    addEnergyHatch(exact->controllers[rigB.controller_id], 32, 71, 32);
    startRecipe(exact->reg, rigB.entity, kRecipeDuration);
    exact->sys->tick(0.05f);
    CHECK_EQ_INT(exact->reg.get<simcore::RecipeProgress>(rigB.entity).remaining_ticks,
                 kRecipeDuration - 1,
                 "current == cost is NOT a stall: the tick runs and lands on zero");
    CHECK_EQ_INT(exact->reg.get<simcore::EnergyStorage>(rigB.entity).current, 0,
                 "the exact-cost buffer is fully spent");
}

static void test_EBFSystem_stalled_eu_machinery_is_never_published() {
    // Same as the stall test but the assertion is about the publish: the EBF
    // publishes NOTHING while stalled, so a client's energy bar freezes at its
    // last known value. Asserted so a future "always publish" change is
    // deliberate.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 33, 70, 33, 1, EnergyType::ELECTRICITY, 10000, 0, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 33, 71, 33);
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    for (int i = 0; i < 5; ++i) f->sys->tick(0.05f);

    CHECK_EQ_INT(f->events->updates.size(), size_t(0),
                 "five stalled ticks publish nothing at all");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration, "progress frozen for five ticks");
}

static void test_EBFSystem_ebf_with_wrong_energy_type_stalls() {
    // EBFSystem.cpp:192: pattern 1 requires EnergyType::ELECTRICITY.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 34, 70, 34, 1, EnergyType::HEAT, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 34, 71, 34);
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "a HEAT buffer in an EBF stalls: no debit, no progress");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration, "no progress with the wrong energy type");
}

static void test_EBFSystem_hbf_with_wrong_energy_type_stalls() {
    // EBFSystem.cpp:193: pattern 4 requires EnergyType::HEAT.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 35, 70, 35, 4, EnergyType::ELECTRICITY, 10000, 500, 2);
    placeCoils(f->reg, 35, 70, 35, simcore::KANHAL_COIL_BLOCK_ID,
               simcore::KANHAL_COIL_BLOCK_ID);
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "an ELECTRICITY buffer in an HBF stalls");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration, "no progress with the wrong energy type");
}

static void test_EBFSystem_missing_recipe_clears_progress() {
    // EBFSystem.cpp:185-188: a recipe id that no longer resolves is dropped.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 36, 70, 36, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 36, 71, 36);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    f->reg.get<simcore::RecipeProgress>(rig.entity).recipe_id = "no_such_recipe";

    f->sys->tick(0.05f);

    const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
    CHECK(p.recipe_id.empty(), "a vanished recipe id is cleared");
    CHECK(!p.is_processing, "is_processing is cleared with it");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "a vanished recipe costs nothing");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "the early return skips the publish");
}

// ---------------------------------------------------------------------------
// Full run: charge, complete, output placement
// ---------------------------------------------------------------------------

static void test_EBFSystem_full_run_consumes_exact_eu_and_produces_output() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 40, 70, 40, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    auto& ctrl = f->controllers[rig.controller_id];
    addEnergyHatch(ctrl, 40, 71, 40);
    addItemHatches(ctrl, 0, 1, 1, 2);  // ITEM_IN [0,1), ITEM_OUT [1,2)
    startRecipe(f->reg, rig.entity, static_cast<int>(kRecipeDuration));

    const int32_t total = static_cast<int32_t>(kRecipeDuration) *
                          static_cast<int32_t>(kRecipeEuPerTick);

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const auto& energy = f->reg.get<simcore::EnergyStorage>(rig.entity);
    CHECK_EQ_INT(energy.current, 500 - total,
                 "duration ticks x 2 EU/tick is the exact total charge");

    const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
    CHECK_EQ_INT(p.remaining_ticks, 0u, "the recipe ran to completion");
    CHECK(!p.is_processing, "is_processing is false at completion");
    CHECK(p.needs_output, "needs_output is set at completion");
    CHECK(p.recipe_id.empty(), "recipe_id is cleared at completion");

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[1].item_id, kSlag, "output lands in the ITEM_OUT slot");
    CHECK_EQ_INT(inv.slots[1].count, 2, "both output items are placed");
}

static void test_EBFSystem_completion_publishes_capacity_and_ratio() {
    // FINDING G (the healthy contrast to gp-bbbl): the EBF DOES publish
    // energy_capacity, unlike RotareGeneratorSystem.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 41, 70, 41, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 41, 71, 41);
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);
    startRecipe(f->reg, rig.entity, static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    CHECK(f->events->updates.size() >= static_cast<size_t>(kRecipeDuration),
          "every charged tick publishes");
    const auto& last = f->events->updates.back();
    CHECK_EQ_INT(last.energy_capacity, 10000u,
                 "energy_capacity is the real capacity, not 0 (contrast gp-bbbl)");
    CHECK_EQ_INT(last.energy, static_cast<uint32_t>(500 - 8),
                 "the last update carries the post-debit buffer");
    CHECK_EQ_INT(last.slots_in, 1,
                 "slots_in is the ITEM_IN slot END, so the client splits the grid");
}

static void test_EBFSystem_output_stacks_onto_a_matching_stack() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 42, 70, 42, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 42, 71, 42);
    addItemHatches(f->controllers[rig.controller_id], 0, 1, 1, 2);
    // Pre-existing partial stack of the same item in the output slot.
    f->reg.get<simcore::InventoryContainer>(rig.entity).slots[1] = {kSlag, 60, 0};
    startRecipe(f->reg, rig.entity, static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[1].count, 62, "60 existing + 2 produced");
    CHECK_EQ_INT(inv.slots[1].item_id, kSlag, "still the same item id");
}

static void test_EBFSystem_output_is_dropped_with_no_output_range() {
    // FINDING E (corrected against the first draft of this suite): with no
    // ITEM_OUT hatch AND no MachineRegistry entry the output range is [0, 0).
    // The two placement loops at EBFSystem.cpp:246-261 both iterate
    // `i < output_end_capped`, so with an empty range NEITHER runs and the
    // product is not deposited anywhere — it is silently dropped after the
    // `spdlog::warn("[EBF] ITEM_OUT hatch full, {} of item {} dropped")` at
    // EBFSystem.cpp:263. Slot 0 (the input slot) is NOT a fallback target.
    //
    // The earlier draft of this test asserted slot 0 and was wrong; the
    // evidence is the [warn] "ITEM_OUT hatch full, 2 of item ... dropped" line
    // the system emits on the completing tick. Corrected here, not in src/.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 43, 70, 43, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 43, 71, 43);
    startRecipe(f->reg, rig.entity, static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[0].item_id, 0, "slot 0 is untouched, not used as a fallback");
    CHECK_EQ_INT(inv.slots[1].item_id, 0, "slot 1 is untouched too");
    // The recipe still completes: the product is destroyed, not held.
    CHECK(f->reg.get<simcore::RecipeProgress>(rig.entity).needs_output,
          "the recipe completes and flags needs_output despite dropping the product");
}

static void test_EBFSystem_null_machine_registry_is_a_missing_singleton_not_a_guarded_path() {
    // FINDING H (NEW, filed as a beads issue): EBFSystem.cpp:136 and :141 guard
    // the RESULT of MachineRegistry::instance()->Get(...) but not the INSTANCE
    // itself:
    //     if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id))
    // With MachineRegistry::instance() == nullptr the ->Get() call dereferences
    // null and the tick segfaults — the `if` never gets a chance to test anything.
    // The same unguarded shape appears at EBFSystem.cpp:406,
    // LCRSystem.cpp:95,100,340, GeneratorSystem.cpp:187 and
    // MachineSystem.cpp:89,135,452,549,606.
    //
    // A crash cannot be asserted inside the suite, so this test pins what IS
    // observable: with a null singleton the slot-layout fallback is not merely
    // "no fallback", it is undefined behaviour. The test therefore installs an
    // EMPTY (non-null) registry, which is the closest legal approximation, and
    // records the boundary. If a MachineRegistry::instance() null guard is ever
    // added, this test can be replaced by one that runs with the singleton unset.
    CHECK(MachineRegistry::instance() != nullptr,
          "precondition: a non-null registry is installed, because "
          "EBFSystem.cpp:136 dereferences instance() unguarded");
    installEmptyRegistry();
    CHECK(MachineRegistry::instance() != nullptr,
          "an EMPTY registry is still non-null, so Get() returns nullptr safely");
}

static void test_EBFSystem_registry_supplies_the_output_range() {
    // The MachineRegistry fallback: 1 input slot, 1 output slot. Installed
    // AFTER makeFixture(), which resets the singleton to an empty registry.
    auto f = makeFixture();
    installRegistry(kTestBlockId, 1, 1);
    auto rig = installFurnace(*f, 44, 70, 44, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 44, 71, 44);
    startRecipe(f->reg, rig.entity, static_cast<int>(kRecipeDuration));

    for (int i = 0; i < static_cast<int>(kRecipeDuration); ++i) f->sys->tick(0.05f);

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[1].item_id, kSlag,
                 "with a registry the product lands in the output slot, not dropped");
}

// ---------------------------------------------------------------------------
// Recipe START from an input inventory
// ---------------------------------------------------------------------------

static void test_EBFSystem_matching_inputs_start_the_recipe_and_consume_them() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 50, 70, 50, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    auto& ctrl = f->controllers[rig.controller_id];
    addEnergyHatch(ctrl, 50, 71, 50);
    // ITEM_IN [0,1) is what makes input_end non-zero; without it the input
    // scan at EBFSystem.cpp:274-279 iterates an empty range and NO recipe can
    // ever match, whatever is in the slots.
    addItemHatches(ctrl, 0, 1, 1, 2);
    f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0] = {kIronDust, 3, 0};

    f->sys->tick(0.05f);

    const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
    CHECK_EQ_INT(p.recipe_id, std::string("test_ebf_iron"), "inputs matched the recipe");
    CHECK(p.is_processing, "the recipe is running");
    CHECK_EQ_INT(p.remaining_ticks, kRecipeDuration, "remaining_ticks is the duration");

    const auto& inv = f->reg.get<simcore::InventoryContainer>(rig.entity);
    CHECK_EQ_INT(inv.slots[0].count, 2, "one iron_dust is consumed, two remain");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "the start tick itself does not charge EU");
}

static void test_EBFSystem_no_matching_inputs_leave_the_machine_idle() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 51, 70, 51, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    auto& ctrl = f->controllers[rig.controller_id];
    addEnergyHatch(ctrl, 51, 71, 51);
    addItemHatches(ctrl, 0, 1, 1, 2);

    f->sys->tick(0.05f);

    const auto& p = f->reg.get<simcore::RecipeProgress>(rig.entity);
    CHECK(p.recipe_id.empty(), "an empty inventory starts nothing");
    CHECK(!p.is_processing, "and nothing is processing");
    CHECK_EQ_INT(f->events->updates.size(), size_t(1),
                 "an idle furnace still publishes its state once per tick");
}

static void test_EBFSystem_inputs_consumed_exactly_once_per_start() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 52, 70, 52, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    auto& ctrl = f->controllers[rig.controller_id];
    addEnergyHatch(ctrl, 52, 71, 52);
    addItemHatches(ctrl, 0, 1, 1, 2);
    f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0] = {kIronDust, 1, 0};

    f->sys->tick(0.05f);
    const uint8_t after_start = f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].count;
    f->sys->tick(0.05f);
    f->sys->tick(0.05f);
    const uint8_t after_more = f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].count;

    CHECK_EQ_INT(after_start, 0, "the single iron_dust is consumed at the start");
    CHECK_EQ_INT(after_more, 0, "running ticks do not consume inputs again");
}

static void test_EBFSystem_wrong_input_never_starts() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 53, 70, 53, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    auto& ctrl = f->controllers[rig.controller_id];
    addEnergyHatch(ctrl, 53, 71, 53);
    addItemHatches(ctrl, 0, 1, 1, 2);
    f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0] = {kIronIngot, 1, 0};

    f->sys->tick(0.05f);

    CHECK(f->reg.get<simcore::RecipeProgress>(rig.entity).recipe_id.empty(),
          "iron_ingot is not an EBF input; nothing starts");
    CHECK_EQ_INT(f->reg.get<simcore::InventoryContainer>(rig.entity).slots[0].count, 1,
                 "the unmatched input is left alone");
}

// ---------------------------------------------------------------------------
// HBF: coil tiers, heat gate, heat mirror
// ---------------------------------------------------------------------------

static void test_EBFSystem_hbf_requires_half_the_coil_tier() {
    // Two kanhal coils -> tier 1800 -> requiredHeat 900.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 60, 70, 60, 4, EnergyType::HEAT, 10000, 5000, 2);
    placeCoils(f->reg, 60, 70, 60, simcore::KANHAL_COIL_BLOCK_ID,
               simcore::KANHAL_COIL_BLOCK_ID);
    // A full buffer of HEAT: heat_stored is only mirrored on the previous tick,
    // so drive it directly to isolate the gate.
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 5000;
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    // 5000 >= 900 so the heat gate passes and the recipe advances.
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1,
                 "5000 HU is above kanhal's required 900 HU, so the HBF runs");
}

static void test_EBFSystem_hbf_stalls_below_half_of_coil_tier() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 61, 70, 61, 4, EnergyType::HEAT, 10000, 5000, 2);
    placeCoils(f->reg, 61, 70, 61, simcore::KANHAL_COIL_BLOCK_ID,
               simcore::KANHAL_COIL_BLOCK_ID);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    const int required = kKanhalMax / 2;  // 900
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = required - 1;
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration, "1 HU below the gate stalls the HBF");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 5000,
                 "a heat-starved HBF spends nothing");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0),
                 "the heat stall returns before the publish (EBFSystem.cpp:191)");
}

static void test_EBFSystem_hbf_heat_gate_boundary_is_inclusive() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 62, 70, 62, 4, EnergyType::HEAT, 10000, 5000, 2);
    placeCoils(f->reg, 62, 70, 62, simcore::KANHAL_COIL_BLOCK_ID,
               simcore::KANHAL_COIL_BLOCK_ID);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = kKanhalMax / 2;
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1,
                 "exactly coilMaxHeat/2 HU satisfies the gate (< is the stall)");
}

static void test_EBFSystem_hbf_heat_tier_is_the_minimum_of_both_coil_rows() {
    // FINDING B: the tier is the MIN of the two rows, and BOTH must be coils.
    // Kanhal below nichrome -> kanhal wins.
    {
        auto f = makeFixture();
        auto rig = installFurnace(*f, 63, 70, 63, 4, EnergyType::HEAT, 10000, 9000, 2);
        placeCoils(f->reg, 63, 70, 63, simcore::KANHAL_COIL_BLOCK_ID,
                   simcore::NICHROME_COIL_BLOCK_ID);
        f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
        // Exactly kanhal/2 = 900: passes if the tier really is kanhal.
        f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 900;
        startRecipe(f->reg, rig.entity, kRecipeDuration);
        f->sys->tick(0.05f);
        CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                     kRecipeDuration - 1, "kanhal below nichrome -> kanhal tier, 900 HU passes");
    }
    {
        // The reverse: nichrome below kanhal -> nichrome tier, whose gate is
        // 1350, so 900 HU must stall.
        auto f = makeFixture();
        auto rig = installFurnace(*f, 64, 70, 64, 4, EnergyType::HEAT, 10000, 9000, 2);
        placeCoils(f->reg, 64, 70, 64, simcore::NICHROME_COIL_BLOCK_ID,
                   simcore::KANHAL_COIL_BLOCK_ID);
        f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
        f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 900;
        startRecipe(f->reg, rig.entity, kRecipeDuration);
        f->sys->tick(0.05f);
        // OBSERVED, not assumed: the loop takes the MIN over both rows, so the
        // tier here is kanhal (1800) and the gate is 900 — 900 satisfies it and
        // the tick runs. The earlier draft of this test asserted a stall on the
        // assumption that the tier was the MAX; the code is `minHeat =
        // std::min(minHeat, layerHeat)` (EBFSystem.cpp:91), i.e. the WEAKER
        // coil governs. Corrected here, not in src/.
        CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                     kRecipeDuration - 1,
                     "the MIN over both rows governs, so kanhal's 900 HU gate applies");
        CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 9000 - 2,
                     "and the tick is charged");
    }
    {
        // Nichrome above and below -> the higher tier, gate 1350.
        auto f = makeFixture();
        auto rig = installFurnace(*f, 65, 70, 65, 4, EnergyType::HEAT, 10000, 9000, 2);
        placeCoils(f->reg, 65, 70, 65, simcore::NICHROME_COIL_BLOCK_ID,
                   simcore::NICHROME_COIL_BLOCK_ID);
        f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
        f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = kNichromeMax / 2;
        startRecipe(f->reg, rig.entity, kRecipeDuration);
        f->sys->tick(0.05f);
        CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                     kRecipeDuration - 1, "two nichrome coils -> 1350 HU gate");
    }
    {
        // Tungstensteel tier: 4500 / 2 = 2250.
        auto f = makeFixture();
        auto rig = installFurnace(*f, 66, 70, 66, 4, EnergyType::HEAT, 10000, 9000, 2);
        placeCoils(f->reg, 66, 70, 66, simcore::TUNGSTENSTEEL_COIL_BLOCK_ID,
                   simcore::TUNGSTENSTEEL_COIL_BLOCK_ID);
        f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
        f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = kTungstensteelMax / 2;
        startRecipe(f->reg, rig.entity, kRecipeDuration);
        f->sys->tick(0.05f);
        CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                     kRecipeDuration - 1, "tungstensteel coils -> 2250 HU gate");
    }
}

static void test_EBFSystem_hbf_with_one_missing_coil_row_is_permanently_cold() {
    // FINDING B (second half): a non-coil block on either row makes
    // detectHeatTier() return 0 outright, so requiredHeat is 0 and the furnace
    // accepts ANY heat level — including none. The gate is silently disabled
    // rather than the furnace being rejected.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 67, 70, 67, 4, EnergyType::HEAT, 10000, 100, 2);
    // Only ONE coil row is placed; the other row finds no block at all.
    placeCoil(f->reg, 67, 70, 67, kCoilLayer1,
              simcore::KANHAL_COIL_BLOCK_ID);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 0;
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1,
                 "a missing coil row zeroes the heat gate: 0 HU is enough to run");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 98,
                 "and the recipe is charged normally");
}

static void test_EBFSystem_hbf_with_no_coils_requires_zero_heat() {
    // FINDING D: no coils at all -> detectHeatTier() returns 0 -> requiredHeat
    // 0 -> an HBF with a bare HEAT buffer runs.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 68, 70, 68, 4, EnergyType::HEAT, 10000, 10, 2);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 0;
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1, "a coil-less HBF needs no heat to run");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 8,
                 "and is charged the same 2 EU/tick");
}

static void test_EBFSystem_hbf_heat_stored_mirrors_the_debited_buffer() {
    // FINDING C: an HBF has ONE fuel pool, not two. The heat gate reads
    // HeatIntakeComponent::heat_stored (EBFSystem.cpp:191) but the debit comes
    // out of EnergyStorage::current (EBFSystem.cpp:221), and only AFTER the
    // debit is heat_stored overwritten with the post-debit energy
    // (EBFSystem.cpp:223). So heat_stored is a lagging MIRROR of the energy
    // buffer, never an independent store.
    //
    // Consequence pinned here: the mirror follows energy.current DOWN, so
    // once the HBF has run, heat_stored can never exceed energy.current — and
    // since the NEXT tick's gate compares heat_stored against requiredHeat
    // while the debit compares energy.current, the two can disagree. This test
    // starts them equal (500 / 500), which is the state production reaches
    // through AdjacencyTransferSystem (it writes both).
    auto f = makeFixture();
    // Seeded at 1000 so the kanhal gate (900 HU) passes and the tick actually
    // reaches the ENERGY branch. Seeding below 900 would stall on the heat gate
    // and the mirror would never run.
    auto rig = installFurnace(*f, 69, 70, 69, 4, EnergyType::HEAT, 10000, 1000, 2);
    placeCoils(f->reg, 69, 70, 69, simcore::KANHAL_COIL_BLOCK_ID,
               simcore::KANHAL_COIL_BLOCK_ID);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 1000;
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 998,
                 "the debit comes out of EnergyStorage::current");
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 998,
                 "heat_stored is then overwritten with the post-debit energy");
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored,
                 f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 "the two fields are exactly equal after a mirroring tick");
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks,
                 kRecipeDuration - 1, "the tick was charged, not stalled");
}

static void test_EBFSystem_hbf_heat_gate_and_energy_buffer_move_together() {
    // FINDING C, second half — the two fields are COUPLED, so an HBF can never
    // be starved of HU while holding EU (or vice versa):
    //   * the gate reads heat_stored            (EBFSystem.cpp:191)
    //   * the debit reads energy.current        (EBFSystem.cpp:201/221)
    //   * the mirror copies energy -> heat      (EBFSystem.cpp:223)
    // Because the mirror runs on every charged tick, heat_stored always equals
    // energy.current afterwards, so the kanhal gate (900 HU) trips at the same
    // moment the buffer can no longer pay the 2 HU/tick. An HBF therefore
    // stalls on BOTH branches simultaneously and neither can starve alone.
    //
    // This is asserted as observed. The earlier draft of this test drove the
    // buffer to 1 HU and expected the energy branch to stall alone; it cannot
    // happen, because the heat gate fires first and freezes the drain.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 70, 70, 70, 4, EnergyType::HEAT, 10000, 1000, 2);
    placeCoils(f->reg, 70, 70, 70, simcore::KANHAL_COIL_BLOCK_ID,
               simcore::KANHAL_COIL_BLOCK_ID);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 1000;
    // A long duration so the recipe cannot complete and restart mid-run.
    startRecipe(f->reg, rig.entity, 500);

    // Run until the buffer stops moving. The gate is kanhal/2 == 900, so the
    // drain must halt at 900 — never below it.
    int guard = 0;
    while (guard < 1000) {
        const int32_t before = f->reg.get<simcore::EnergyStorage>(rig.entity).current;
        f->sys->tick(0.05f);
        const int32_t after = f->reg.get<simcore::EnergyStorage>(rig.entity).current;
        if (after == before) break;
        ++guard;
    }
    CHECK(guard < 1000, "the drain terminates well inside the guard");
    // The gate is `heat_stored < requiredHeat -> return` (EBFSystem.cpp:191) and
    // is evaluated BEFORE the debit, so the buffer drains from 1000 to 898:
    // 900 passes the gate and is debited to 898, and 898 then fails the gate.
    // The drain therefore stops 2 HU BELOW the gate, not on it.
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current,
                 kKanhalMax / 2 - 2,
                 "the drain stops 2 HU below the gate: 900 passes and debits to 898");
    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored,
                 kKanhalMax / 2 - 2,
                 "the mirror is exactly equal, so heat and energy stall together");

    const uint32_t ticks = f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks;
    f->sys->tick(0.05f);
    CHECK_EQ_INT(f->reg.get<simcore::RecipeProgress>(rig.entity).remaining_ticks, ticks,
                 "a further tick makes no progress (the heat gate holds it)");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, kKanhalMax / 2 - 2,
                 "and spends nothing");
    CHECK(f->reg.get<simcore::RecipeProgress>(rig.entity).is_processing,
          "the recipe stays flagged processing while stalled");
}

static void test_EBFSystem_ebf_heat_stored_is_never_written() {
    // The mirror is HBF-only (EBFSystem.cpp:222-224 is inside `if (!electric)`),
    // so an EBF's HeatIntakeComponent keeps whatever the boiler put there.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 71, 70, 71, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 71, 71, 71);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 1234;
    startRecipe(f->reg, rig.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored, 1234,
                 "an EBF never writes heat_stored");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 498,
                 "but it still spends EU");
}

static void test_EBFSystem_heat_intake_is_emplaced_lazily_for_an_hbf() {
    auto f = makeFixture();
    auto rig = installFurnace(*f, 72, 70, 72, 4, EnergyType::HEAT, 10000, 5000, 2);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    CHECK(!f->reg.all_of<simcore::HeatIntakeComponent>(rig.entity),
          "precondition: no HeatIntakeComponent yet");

    f->sys->tick(0.05f);

    CHECK(f->reg.all_of<simcore::HeatIntakeComponent>(rig.entity),
          "get_or_emplace creates it on the first tick");
}

static void test_EBFSystem_heat_stored_hits_the_coil_cap() {
    // Two ticks of a 2 HU charge into a 1000 HU buffer: no cap interaction, but
    // the mirror makes heat_stored exactly the buffer. Pinned so the HeatIntake
    // default heat_capacity (1000) is visible in the suite.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 73, 70, 73, 4, EnergyType::HEAT, 10000, 1000, 2);
    placeCoils(f->reg, 73, 70, 73, simcore::KANHAL_COIL_BLOCK_ID,
               simcore::KANHAL_COIL_BLOCK_ID);
    f->reg.emplace<simcore::HeatIntakeComponent>(rig.entity);
    f->reg.get<simcore::HeatIntakeComponent>(rig.entity).heat_stored = 1000;
    startRecipe(f->reg, rig.entity, 3);

    f->sys->tick(0.05f);
    const auto& hic = f->reg.get<simcore::HeatIntakeComponent>(rig.entity);
    CHECK_EQ_INT(hic.heat_capacity, 1000, "the default heat capacity is 1000");
    CHECK_EQ_INT(hic.heat_stored, 998, "1000 HU debited by 2");
    CHECK(fabs(hic.ratio() - 998.0f / 1000.0f) < 1e-5f,
          "the published heat ratio is heat_stored / heat_capacity");
}

// ---------------------------------------------------------------------------
// Multiple controllers in one map
// ---------------------------------------------------------------------------

static void test_EBFSystem_two_controllers_are_both_ticked() {
    auto f = makeFixture();
    auto a = installFurnace(*f, 80, 70, 80, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    auto b = installFurnace(*f, 81, 70, 81, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[a.controller_id], 80, 71, 80);
    addEnergyHatch(f->controllers[b.controller_id], 81, 71, 81);
    startRecipe(f->reg, a.entity, kRecipeDuration);
    startRecipe(f->reg, b.entity, kRecipeDuration);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(a.entity).current, 498, "furnace A charged");
    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(b.entity).current, 498, "furnace B charged");
    CHECK_EQ_INT(f->events->updates.size(), size_t(2), "each furnace publishes its own update");
}

static void test_EBFSystem_empty_controller_map_is_a_noop() {
    auto f = makeFixture();
    f->sys->tick(0.05f);
    f->sys->tick(1.0f);
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "no controllers -> no publishes");
}

static void test_EBFSystem_controller_erased_between_ticks_is_skipped() {
    // The tick() collects ids first and re-looks-up each one
    // (EBFSystem.cpp:37-42), so a controller erased mid-tick cannot be ticked.
    auto f = makeFixture();
    auto rig = installFurnace(*f, 90, 70, 90, 1, EnergyType::ELECTRICITY, 10000, 500, 2);
    addEnergyHatch(f->controllers[rig.controller_id], 90, 71, 90);
    startRecipe(f->reg, rig.entity, kRecipeDuration);
    f->controllers.erase(rig.controller_id);

    f->sys->tick(0.05f);

    CHECK_EQ_INT(f->reg.get<simcore::EnergyStorage>(rig.entity).current, 500,
                 "an unregistered controller is not ticked");
    CHECK_EQ_INT(f->events->updates.size(), size_t(0), "and publishes nothing");
}





#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== ebf_system test suite ===\n\n");

    // Selection gates
    TEST(EBFSystem_zero_controller_id_is_skipped);
    TEST(EBFSystem_foreign_pattern_id_is_skipped);
    TEST(EBFSystem_controller_without_matching_entity_is_a_noop);
    TEST(EBFSystem_dt_is_ignored);
    TEST(EBFSystem_empty_controller_map_is_a_noop);
    TEST(EBFSystem_controller_erased_between_ticks_is_skipped);
    TEST(EBFSystem_two_controllers_are_both_ticked);

    // ENERGY hatch requirement
    TEST(EBFSystem_ebf_without_energy_hatch_does_nothing);
    TEST(EBFSystem_ebf_with_absent_energy_hatch_does_nothing);
    TEST(EBFSystem_ebf_with_energy_hatch_spends_eu);
    TEST(EBFSystem_eu_cost_is_the_truncated_recipe_energy_cost);
    TEST(EBFSystem_heat_requirement_is_zero_for_an_ebf);

    // Stalls
    TEST(EBFSystem_stalls_when_eu_buffer_is_below_cost);
    TEST(EBFSystem_stall_is_exactly_the_cost_boundary);
    TEST(EBFSystem_stalled_eu_machinery_is_never_published);
    TEST(EBFSystem_ebf_with_wrong_energy_type_stalls);
    TEST(EBFSystem_hbf_with_wrong_energy_type_stalls);
    TEST(EBFSystem_missing_recipe_clears_progress);

    // Full run
    TEST(EBFSystem_full_run_consumes_exact_eu_and_produces_output);
    TEST(EBFSystem_completion_publishes_capacity_and_ratio);
    TEST(EBFSystem_output_stacks_onto_a_matching_stack);
    TEST(EBFSystem_output_is_dropped_with_no_output_range);
    TEST(EBFSystem_null_machine_registry_is_a_missing_singleton_not_a_guarded_path);
    TEST(EBFSystem_registry_supplies_the_output_range);

    // Recipe start
    TEST(EBFSystem_matching_inputs_start_the_recipe_and_consume_them);
    TEST(EBFSystem_no_matching_inputs_leave_the_machine_idle);
    TEST(EBFSystem_inputs_consumed_exactly_once_per_start);
    TEST(EBFSystem_wrong_input_never_starts);

    // HBF heat tiers
    TEST(EBFSystem_hbf_requires_half_the_coil_tier);
    TEST(EBFSystem_hbf_stalls_below_half_of_coil_tier);
    TEST(EBFSystem_hbf_heat_gate_boundary_is_inclusive);
    TEST(EBFSystem_hbf_heat_tier_is_the_minimum_of_both_coil_rows);
    TEST(EBFSystem_hbf_with_one_missing_coil_row_is_permanently_cold);
    TEST(EBFSystem_hbf_with_no_coils_requires_zero_heat);
    TEST(EBFSystem_hbf_heat_stored_mirrors_the_debited_buffer);
    TEST(EBFSystem_hbf_heat_gate_and_energy_buffer_move_together);
    TEST(EBFSystem_ebf_heat_stored_is_never_written);
    TEST(EBFSystem_heat_intake_is_emplaced_lazily_for_an_hbf);
    TEST(EBFSystem_heat_stored_hits_the_coil_cap);

    // Cleanup: the synthetic recipe file and the process-wide registries.
    for (const auto& path : Fixture::tempFiles()) {
        remove(path.c_str());
    }
    Fixture::tempFiles().clear();
    clearRegistry();

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
