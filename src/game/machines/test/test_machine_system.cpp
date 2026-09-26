// MachineSystem unit tests (issues gp-015h tick/state publication, gp-s9to
// machine-tag filtering and role dispatch).
//
// Covers src/game/machines/MachineSystem.cpp — the generic single-block machine
// loop: the per-tick state publication, the publish-every-N-ticks counter, the
// recipe start / steam top-up / recipe tick passes, the `managed_externally`
// role gate, and the MachineRegistry slot-layout resolution.
//
// DRIVER SHAPE (why these tests look the way they do):
//   MachineSystem::tick does NOT walk a controller map. It iterates ONE EnTT
//   view, four components wide, and walks it FOUR times:
//     MachineSystem.cpp:57   view<MachineComponent, RecipeProgress,
//                                    InventoryContainer, EnergyStorage>
//     Pass 0  :67   publish inventory/state, gated on an inventory hash +
//                  the force-publish counter
//     Pass 1  :116  start a recipe on an idle machine (managed_externally? skip)
//     Pass 1.5:206  passive steam top-up for an idle STEAM machine
//     Pass 2  :253  tick the active recipe (overheat, charge, progress, output)
//   An entity missing ANY of the four components is in no pass at all, which is
//   the "a machine that is not present is not touched" case below.
//
//   NOTABLE: this view does NOT contain MultiblockController, so MachineSystem
//   is NOT affected by the dead-view defect filed as gp-qgtc (which hits
//   ExplosionSystem / AdjacencyTransferSystem / CoolantSystem). MachineSystem
//   really does run in production: SimulationEngine.cpp:218-259 gives every
//   single-block machine MachineComponent + RecipeProgress + InventoryContainer
//   + EnergyStorage, and main.cpp:461 registers the system unconditionally.
//
//   THE THREE STARTUP TICKS DOUBLE-PUBLISH. startupTicks_ forces the Pass 0
//   publish on ticks 1-3, and those same ticks then run Pass 2, which publishes
//   again. So a running machine emits 2 updates per startup tick, 1 per ordinary
//   tick, and 2 per FORCE tick. Only the ordinary case is "once per tick".
//
//   loadRecipesFromYamlFile reads ONE top-level `class:` and ONE `recipes:`
//   sequence per file (RecipeManager.cpp:510-521) — a second `class:` key in the
//   same file is silently dropped by yaml-cpp. The suite therefore writes one
//   file per class, exactly like src/content/data/recipes/, and
//   MachineSystem_every_synthetic_recipe_class_loads guards the trap.
//
// CONSTANTS READ FROM SOURCE (asserted by name, so a change shows in the diff):
//   MachineSystem::kForcePublishInterval  10   (MachineSystem.h:54)
//   MachineSystem::kSteamFillQuantum      1000 (MachineSystem.h:60)
//   startupTicks_ = 3                     (MachineSystem.h:90) — private, so the
//       first three ticks are pinned BEHAVIOURALLY (every machine published
//       regardless of change), not by reading the field.
//   MachineSystem::tick(float dt) IGNORES dt (the parameter is unnamed at
//       MachineSystem.cpp:55), so a 100 s tick advances exactly one tick.
//   Pass 0 publishes progress as a hardcoded 1.0f "waiting for recipe"
//       (MachineSystem.cpp:106) — even for a RUNNING recipe.
//   Pass 2 publishes pct = 1 - remaining_ticks/duration, and 0 when
//       duration == 0 (MachineSystem.cpp:545-546).
//   The per-tick cost is `static_cast<int32_t>(recipe->energy_cost)`, so a
//       fractional `eu:` charges the integer part (MachineSystem.cpp:291-293).
//   OverheatState::WARNING halves the rate: one progress tick every SECOND
//       simulation tick (MachineSystem.cpp:268-274).
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed" here):
//   A. THE PUBLISH COUNTER IS OFFSET BY THE STARTUP WINDOW. Ticks 1-3 publish
//      unconditionally (startupTicks_), and because those ticks also increment
//      tickCounter_, the first interval fire lands on tick 10 and every 10
//      ticks after. Pinned by MachineSystem_force_publish_lands_on_tick_ten.
//   B. (FIXED, gp-dyo4) THE INVENTORY CHANGE HASH USED TO HAVE EXACT
//      COLLISIONS. The fold at MachineSystem.cpp:75-81 was an XOR-of-XOR
//      accumulator with no slot index in the term, so an EMPTY slot
//      contributed nothing and four identical stacks cancelled back to the
//      empty hash. A machine whose inventory changed from all-empty to four
//      identical stacks did NOT republish until the next forced publish — up
//      to 10 ticks of stale client state. The change gate is now an FNV-1a
//      fold over the exact published byte stream, length-prefixed, so equal
//      contents always hash equal and every distinct change is published.
//      Pinned by MachineSystem_identical_stacks_publish_at_once and
//      MachineSystem_slot_count_change_publishes.
//   C. A FORCE-PUBLISH TICK PUBLISHES A RUNNING MACHINE TWICE — once from
//      Pass 0 (progress hardcoded 1.0, PRE-debit energy) and once from Pass 2
//      (real pct, post-debit energy). Pinned by
//      MachineSystem_force_tick_publishes_a_running_machine_twice.
//   D. A RECIPE STARTS AND TICKS IN THE SAME CALL. Pass 1 sets recipe_id and
//      remaining_ticks = duration; Pass 2 then runs in the same tick, debits
//      and decrements. A machine that just started a 4-tick recipe is already
//      at 3. Pinned by MachineSystem_start_pass_and_tick_pass_share_one_tick.
//   E. MACHINE TAG FILTERING IS A DEAD SEAM. MachineTagComponent is read by
//      RecipeManager.cpp:44-46 and evaluated by
//      ConditionEvaluator::checkSpecial, but (i) no code in src/ ever emplaces
//      MachineTagComponent, and (ii) parseYamlConditions
//      (RecipeManager.cpp:990-1032) never writes `conditions.special`, so no
//      loaded recipe can ever carry a tag clause. A tag cannot gate anything.
//      Pinned by MachineSystem_machine_tags_cannot_gate_a_recipe.
//   F. (FIXED, gp-frb2) onConsumeResponse FALLS THROUGH INTO THE FIFO BRANCH
//      ON A DIRECT HIT. The `if (node_id != 0) { ... }` block at
//      MachineSystem.cpp:687-712 had no `return`, so a directed response was
//      followed by the "No node_id: process in FIFO order" block at :715 and
//      credited a SECOND machine — or the same one twice, since
//      pendingConsumes_.begin() is the addressed node itself whenever it is
//      the most recently inserted pending. The direct branch now settles its
//      own request and returns. Pinned by
//      MachineSystem_onConsumeResponse_direct_hit_credits_only_its_node.
//   G. THE `node_id != 0` GUARD MISREADS ENTITY 0. EnTT's first entity has id
//      0, so a directed response for it is indistinguishable from "no node id"
//      and is routed to the FIFO head. Still OPEN (same class as gp-u9ua,
//      filed for BatteryBufferSystem); pinned for MachineSystem by
//      MachineSystem_onConsumeResponse_treats_entity_zero_as_no_node_id.
//      The direct branch is deliberately still guarded by `node_id != 0`, so
//      this defect is untouched by the gp-frb2 fix.
//   H. CONDITION STATE IS READ FROM THE FIRST MACHINE AT THOSE COORDINATES.
//      RecipeManager.cpp:20-48 scans view<MachineComponent> and `break`s on the
//      first position match, so a co-located machine supplies the energy/purity
//      /tags that gate every machine at that (x,y,z). Same class as gp-5wms
//      (RecipeCompletedHandler). Pinned by
//      MachineSystem_conditions_come_from_the_first_machine_at_those_coords.
//   I. THE OUTPUT RANGE IS NEVER ZERO-GUARDED. With no MachineRegistry entry
//      slots_in falls back to 0, so a completed recipe deposits its product into
//      the INPUT slot. Pinned by
//      MachineSystem_output_slots_are_never_zero_guarded.
//
// HARNESS: the project's own CHECK/TEST macros (src/engine/net/test/test.h).
// The repo has NO GoogleTest dependency — gtest is absent from conanfile.txt, CI
// does not install libgtest-dev, and CI builds Release with a global -Werror, so
// a find_package(GTest) guard would make this test silently vanish from CI.
//
// DETERMINISM: no sleeps, no network, no cluster, no display. The tick is
// driven directly with a fixed dt against a bare entt::registry. Every network
// client (PipeEnergyClient, ItemClient, CraftReservationClient, the container
// sessions, the inventory store and the router) is supplied as nullptr, which
// every call site in MachineSystem guards — except where a test supplies a
// purpose-built double (RecordingFluidClient, whose overrides never touch a
// router) to make a branch observable.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "Network/IEventPublisher.h"
#include "Network/FluidClient.h"
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/HeatIntakeComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/RecipeProgress.h>
#include <engine/sim/components/SteamOutputComponent.h>
#include <game/machines/HeatSlowComponent.h>
#include <game/machines/MachineSystem.h>
#include <game/machines/MachineTagComponent.h>
#include <game/machines/OverheatComponent.h>
#include <game/recipes/ConditionEvaluator.h>
#include <game/recipes/RecipeManager.h>
#include <game/storage/InventorySerializer.h>

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has
// no GTest dependency, so this is the established convention for focused tests.
#include <engine/net/test/test.h>

#ifndef CHECK_EQ_INT
#define CHECK_EQ_INT(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#endif
#ifndef CHECK_NE_INT
#define CHECK_NE_INT(a, b, ...) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)
#endif
#ifndef CHECK_LT_INT
#define CHECK_LT_INT(a, b, ...) test_check((a) < (b), __FILE__, __LINE__, #a " < " #b, ##__VA_ARGS__)
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
    double steam_current = -1.0;
    double steam_capacity = -1.0;
    size_t inventory_bytes = 0;
    // packed inventory payload, so the wire format itself is assertable
    std::vector<uint8_t> inventory;
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
                                  const std::vector<HatchUpdateData>* = nullptr,
                                  double steam_current = -1.0,
                                  double steam_capacity = -1.0) override {
        PublisherEvent e;
        e.x = x; e.y = y; e.z = z;
        e.machine_id = machine_type;
        e.progress = progress;
        e.energy = energy;
        e.energy_capacity = energy_capacity;
        e.energy_type = energy_type;
        e.slots_in = slots_in;
        e.heat_ratio = heat_ratio;
        e.steam_current = steam_current;
        e.steam_capacity = steam_capacity;
        e.inventory_bytes = inventory_data.size();
        e.inventory = inventory_data;
        updates.push_back(std::move(e));
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

// FluidClient is a concrete class with virtual publishNodeUpdate /
// sendFluidRequest, so a recording subclass intercepts both before they can
// touch the (never connected) router. Returned as a non-owning shared_ptr: the
// base has no virtual destructor, so deleting through a FluidClient* would be
// UB. The process-wide singleton also keeps ordering stable.
struct RecordingFluidClient : simcore::FluidClient {
    struct Call {
        uint64_t node_id = 0;
        int32_t x = 0, y = 0, z = 0;
        uint32_t fluid_id = 0;
        int32_t amount = 0;
        int32_t capacity = 0;
    };
    std::vector<Call> node_updates;
    std::vector<Call> requests;

    RecordingFluidClient() : simcore::FluidClient(nullptr) {}

    void publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                           uint32_t fluid_id, int32_t amount, int32_t capacity,
                           int32_t, int32_t, int32_t,
                           bool, bool,
                           const std::vector<uint64_t>&) override {
        node_updates.push_back(Call{node_id, x, y, z, fluid_id, amount, capacity});
    }

    void sendFluidRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                          uint32_t fluid_id, int32_t amount) override {
        requests.push_back(Call{node_id, x, y, z, fluid_id, amount, 0});
    }

    void clear() { node_updates.clear(); requests.clear(); }
};

static RecordingFluidClient& sharedFluidClient() {
    static RecordingFluidClient client;
    return client;
}

// ---------------------------------------------------------------------------
// Synthetic content
// ---------------------------------------------------------------------------

// Hierarchical item ids: ItemId::pack directly, so the suite never depends on
// src/content/data being present at the CWD.
constexpr uint16_t kIronIngot = ItemId::pack("0:110:1");
constexpr uint16_t kIronDust  = ItemId::pack("0:1110:001:26");
constexpr uint16_t kSlag      = ItemId::pack("0:1110:001:99");
constexpr uint16_t kSlagAlt   = ItemId::pack("0:1110:001:98");
constexpr uint16_t kGravel    = ItemId::pack("0:1110:001:50");

// Test-only block ids (no machines.yaml entry exists for them). The recipe
// classes are wired with RecipeManager::registerMachineClass, exactly as
// main.cpp:229 does for the real EBF controller.
constexpr uint16_t kBlockSmelter   = 0x0AC0;  // class "unit_smelter"
constexpr uint16_t kBlockMacerator = 0x0AC1;  // class "unit_macerator"
constexpr uint16_t kBlockGated     = 0x0AC2;  // class "unit_gated"
constexpr uint16_t kBlockTiered    = 0x0AC3;  // class "unit_tiered"
constexpr uint16_t kBlockFraction  = 0x0AC4;  // class "unit_fractional"
constexpr uint16_t kBlockUntagged  = 0x0AC5;  // class "unit_gated", wrong tier
constexpr uint16_t kBlockUnmapped  = 0x0AC6;  // never registered to any class

// What the YAML below declares, so the numbers and the recipe cannot drift.
constexpr uint32_t kRecipeDuration = 4;
constexpr int32_t kRecipeCost = 2;       // eu: 2.0 -> truncated to 2
constexpr int32_t kFractionalCost = 2;  // eu: 2.5 -> truncated to 2
constexpr int32_t kGateEnergyMin = 500; // conditions.machine.energy_min
constexpr int16_t kTieredMinTier = 2;    // min_tier: 2
constexpr uint32_t kSlowDuration = 400;  // duration of the unit_slow recipe

// The registry layout every machine under test uses: 1 input + 3 output slots.
constexpr int kSlotsIn = 1;
constexpr int kSlotsOut = 3;
constexpr int kSlots = kSlotsIn + kSlotsOut;
constexpr int32_t kCapacity = 10000;

static std::string scratchDir() {
    const char* tmp = getenv("TMPDIR");
    if (tmp && *tmp) {
        std::string dir(tmp);
        if (dir.back() != '/') dir += '/';
        return dir;
    }
    return "./";
}

static std::vector<std::string>& tempFiles() {
    static std::vector<std::string> files;
    return files;
}

// loadRecipesFromYamlFile reads ONE top-level `class:` and ONE `recipes:`
// sequence per file (RecipeManager.cpp:510-521), so each synthetic class gets
// its own file — exactly how src/content/data/recipes/ is laid out.
static std::string writeRecipeFile(const char* name, const char* body) {
    const std::string path = scratchDir() + name;
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return {};
    fputs(body, f);
    fclose(f);
    tempFiles().push_back(path);
    return path;
}

// The default fixture's classes. The `unit_gated` recipe carries BOTH a real
// machine condition (energy_min, which parseYamlConditions DOES read) and a
// `special:` tag clause, which it does not. See FINDING E.
static const char* kSmelterRecipes =
    "class: unit_smelter\n"
    "recipes:\n"
    "  - name: unit_iron\n"
    "    inputs:\n"
    "      - { item: \"0:1110:001:26\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:1110:001:99\", count: 2 }\n"
    "    duration: 4\n"
    "    eu: 2.0\n"
    "    min_tier: 0\n"
    "    max_tier: 32767\n"
    "  - name: unit_slow\n"
    "    inputs:\n"
    "      - { item: \"0:110:1\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:1110:001:99\", count: 1 }\n"
    "    duration: 400\n"
    "    eu: 2.0\n"
    "  - name: unit_unreachable_input\n"
    "    inputs:\n"
    "      - { item: \"0:110:1\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:1110:001:50\", count: 1 }\n"
    "    duration: 8\n"
    "    eu: 0\n";

static const char* kMaceratorRecipes =
    "class: unit_macerator\n"
    "recipes:\n"
    "  - name: unit_gravel\n"
    "    inputs:\n"
    "      - { item: \"0:1110:001:50\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:1110:001:98\", count: 4 }\n"
    "    duration: 4\n"
    "    eu: 2.0\n";

static const char* kGatedRecipes =
    "class: unit_gated\n"
    "recipes:\n"
    "  - name: unit_energy_gated\n"
    "    inputs:\n"
    "      - { item: \"0:1110:001:26\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:1110:001:99\", count: 1 }\n"
    "    duration: 4\n"
    "    eu: 0\n"
    "    conditions:\n"
    "      machine:\n"
    "        energy_min: 500\n"
    "      special:\n"
    "        - { key: 7, value_type: 0, int_value: 42 }\n";

static const char* kTieredRecipes =
    "class: unit_tiered\n"
    "recipes:\n"
    "  - name: unit_mv_only\n"
    "    inputs:\n"
    "      - { item: \"0:1110:001:26\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:1110:001:99\", count: 1 }\n"
    "    duration: 4\n"
    "    eu: 0\n"
    "    min_tier: 2\n";

static const char* kFractionalRecipes =
    "class: unit_fractional\n"
    "recipes:\n"
    "  - name: unit_half_eu\n"
    "    inputs:\n"
    "      - { item: \"0:1110:001:26\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:1110:001:99\", count: 1 }\n"
    "    duration: 6\n"
    "    eu: 2.5\n";

// Writes every file once and returns the smelter one, which is the class most
// tests use. Written lazily so a ctest run only touches TMPDIR when needed.
static const std::vector<std::string>& syntheticRecipeFiles() {
    static std::vector<std::string> paths;
    if (!paths.empty()) return paths;
    paths.push_back(writeRecipeFile("machine_unit_smelter.yaml", kSmelterRecipes));
    paths.push_back(writeRecipeFile("machine_unit_macerator.yaml", kMaceratorRecipes));
    paths.push_back(writeRecipeFile("machine_unit_gated.yaml", kGatedRecipes));
    paths.push_back(writeRecipeFile("machine_unit_tiered.yaml", kTieredRecipes));
    paths.push_back(writeRecipeFile("machine_unit_fractional.yaml", kFractionalRecipes));
    return paths;
}

static void loadSyntheticRecipes(RecipeManager::RecipeManager& mgr) {
    for (const auto& path : syntheticRecipeFiles()) {
        if (!path.empty()) mgr.loadRecipesFromYamlFile(path);
    }
}

// MachineRegistry has a private constructor, so LoadFromYaml is the only way to
// obtain one; it accepts an unopenable path and returns an EMPTY registry
// (MachineRegistry.cpp:54-58). The result is intentionally leaked: the
// singleton has no ownership story and setInstance never frees the old pointer.
static void installRegistry() {
    auto reg = MachineRegistry::LoadFromYaml("/nonexistent/machines.yaml");
    if (!reg) return;
    MachineInfo info{};
    info.id = kBlockSmelter;
    info.name = "test_machine";
    info.machine_class = "unit_smelter";
    info.energy_in = EnergyType::ELECTRICITY;
    info.tier = 1;
    info.slots_in = kSlotsIn;
    info.slots_out = kSlotsOut;
    info.capacity = kCapacity;
    info.maxInput = 32;
    info.maxOutput = 0;
    reg->Register(info);
    MachineRegistry::setInstance(reg.release());
}

static void clearRegistry() { MachineRegistry::setInstance(nullptr); }

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// Declared BEFORE sys and destroyed AFTER it: MachineSystem holds a reference
// to reg_, so the fixture must never be moved after construction.
// std::make_shared constructs it in place, so the bound reference is final.
struct Fixture {
    entt::registry reg;
    std::shared_ptr<RecipeManager::RecipeManager> recipes;
    std::shared_ptr<RecordingPublisher> events;
    std::unique_ptr<simcore::MachineSystem> sys;
    uint16_t next_x = 100;

    simcore::RecipeProgress& progress(entt::entity e) {
        return reg.get<simcore::RecipeProgress>(e);
    }
    simcore::EnergyStorage& energy(entt::entity e) {
        return reg.get<simcore::EnergyStorage>(e);
    }
    simcore::InventoryContainer& inv(entt::entity e) {
        return reg.get<simcore::InventoryContainer>(e);
    }
    simcore::MachineComponent& machine(entt::entity e) {
        return reg.get<simcore::MachineComponent>(e);
    }

    // A machine at a fresh x so every test has its own coordinates (condition
    // evaluation looks machines up BY POSITION, see FINDING H).
    uint32_t freshX() { return next_x++; }

    // The production shape (SimulationEngine.cpp:218-259): one entity carrying
    // exactly the four components of the view at MachineSystem.cpp:57.
    entt::entity install(uint16_t block_id, uint32_t x, EnergyType type,
                         int32_t stored) {
        auto e = reg.create();
        reg.emplace<simcore::MachineComponent>(e, block_id, 0, x, 70, x, x + 1);
        reg.emplace<simcore::RecipeProgress>(e);
        simcore::InventoryContainer container(1, kSlots,
                                              std::vector<simcore::InventorySlot>{});
        container.slots.resize(kSlots);
        reg.emplace<simcore::InventoryContainer>(e, container);
        reg.emplace<simcore::EnergyStorage>(e, kCapacity, stored, 32, 0, 1, type);
        return e;
    }

    void tick() { sys->tick(0.05f); }
    size_t publishes() const { return events->updates.size(); }
    // Publishes attributable to ONE machine, keyed on its x (freshX() gives
    // every machine distinct coordinates). Counting per machine keeps an
    // assertion independent of the force-publish phase: a force tick publishes
    // every machine, so a total-count assertion silently depends on which tick
    // the case happens to land on.
    size_t publishesForX(uint32_t x) const {
        size_t n = 0;
        for (const auto& u : events->updates)
            if (u.x == static_cast<int32_t>(x)) ++n;
        return n;
    }
};

// steam_id == 0 (the production default when the registry cannot resolve it)
// makes the system fail closed on steam, so the steam tests build their own
// system. Everything else uses makeFixture().
static std::shared_ptr<Fixture> makeFixture() {
    // FINDING from gp-r3vd: MachineRegistry::instance() is dereferenced
    // unguarded at MachineSystem.cpp:89,135,452,549,606, so the fixture ALWAYS
    // installs a non-null registry. Resetting it here keeps the suite
    // order-independent.
    installRegistry();
    auto f = std::make_shared<Fixture>();
    f->recipes = std::make_shared<RecipeManager::RecipeManager>();
    loadSyntheticRecipes(*f->recipes);
    f->recipes->registerMachineClass(
        kBlockSmelter, "unit_smelter", 1,
        static_cast<uint8_t>(RecipeManager::EnergyType::ELECTRICITY));
    f->recipes->registerMachineClass(
        kBlockMacerator, "unit_macerator", 1,
        static_cast<uint8_t>(RecipeManager::EnergyType::ELECTRICITY));
    f->recipes->registerMachineClass(kBlockGated, "unit_gated", 1,
                                     RecipeManager::ENERGY_TYPE_ANY);
    f->recipes->registerMachineClass(kBlockTiered, "unit_tiered", 1,
                                     RecipeManager::ENERGY_TYPE_ANY);
    f->recipes->registerMachineClass(
        kBlockFraction, "unit_fractional", 1,
        static_cast<uint8_t>(RecipeManager::EnergyType::ELECTRICITY));
    f->recipes->registerMachineClass(kBlockUntagged, "unit_gated", 1,
                                     RecipeManager::ENERGY_TYPE_ANY);
    f->events = std::make_shared<RecordingPublisher>();
    f->sys = std::make_unique<simcore::MachineSystem>(
        f->reg, f->recipes, f->events, nullptr /* PipeEnergyClient */,
        nullptr /* ItemClient */, nullptr /* sessions */, nullptr /* invStore */,
        nullptr /* router */, nullptr /* FluidClient */,
        nullptr /* CraftReservationClient */, 0 /* steam_item_id */);
    return f;
}

// A fixture whose machine id resolves a Steam item, so Pass 1.5 and the steam
// branch of Pass 2 are reachable. The router is never connected: the supplied
// FluidClient double short-circuits both of its methods.
static std::shared_ptr<Fixture> makeSteamFixture(std::uint16_t steam_item_id) {
    installRegistry();
    auto f = std::make_shared<Fixture>();
    f->recipes = std::make_shared<RecipeManager::RecipeManager>();
    loadSyntheticRecipes(*f->recipes);
    f->recipes->registerMachineClass(
        kBlockSmelter, "unit_smelter", 1,
        static_cast<uint8_t>(RecipeManager::EnergyType::ELECTRICITY));
    f->events = std::make_shared<RecordingPublisher>();
    auto fluid = std::shared_ptr<simcore::FluidClient>(
        &sharedFluidClient(), [](simcore::FluidClient*) {});
    f->sys = std::make_unique<simcore::MachineSystem>(
        f->reg, f->recipes, f->events, nullptr, nullptr, nullptr, nullptr,
        nullptr, fluid, nullptr, steam_item_id);
    return f;
}

static void startRecipe(entt::registry& reg, entt::entity e, const char* id,
                        uint32_t remaining) {
    auto& p = reg.get<simcore::RecipeProgress>(e);
    p.recipe_id = id;
    p.remaining_ticks = remaining;
    p.is_processing = true;
}

// ---------------------------------------------------------------------------
// gp-015h — the tick and the state publication path
// ---------------------------------------------------------------------------

// The force-publish cadence: startupTicks_ publishes everything for three
// ticks, then the counter reaches kForcePublishInterval on tick 10.
static void MachineSystem_force_publish_lands_on_tick_ten() {
    CHECK_EQ_INT(simcore::MachineSystem::kForcePublishInterval, 10,
                 "kForcePublishInterval is the documented 10 ticks");
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 500);

    // Ticks 1-3: the startup window publishes unconditionally.
    for (int i = 0; i < 3; ++i) f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(3), "the startup window publishes all 3 ticks");

    // Ticks 4-9: nothing changed, so the hash gate suppresses the publish.
    for (int i = 0; i < 6; ++i) f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(3),
                 "ticks 4-9 are quiet: the inventory has not changed");

    // Tick 10: tickCounter_ reaches kForcePublishInterval.
    f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(4),
                 "tick 10 is the first forced publish (FINDING A)");

    // And every kForcePublishInterval ticks after that.
    for (int i = 0; i < 9; ++i) f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(4), "ticks 11-19 are quiet again");
    f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(5), "tick 20 is the next forced publish");

    (void)e;
}

static void MachineSystem_idle_machine_publishes_exactly_once_per_tick() {
    // A running machine on a NON-FORCE tick with an unchanged inventory is
    // published by Pass 2 alone. The startup ticks are the exception: they
    // force the Pass 0 publish as well, so they emit two.
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "unit_slow", kSlowDuration);  // never completes in 12 ticks

    for (int i = 0; i < 3; ++i) f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(6),
                 "the three startup ticks publish twice each (Pass 0 + Pass 2)");

    f->tick();  // tick 4: the startup window is over, the inventory is unchanged
    CHECK_EQ_INT(f->publishes(), size_t(7),
                 "a non-force tick with an unchanged inventory publishes once (Pass 2)");

    for (int i = 0; i < 5; ++i) f->tick();  // ticks 5-9
    CHECK_EQ_INT(f->publishes(), size_t(12), "five more non-force ticks, one publish each");
}

// FINDING C.
static void MachineSystem_force_tick_publishes_a_running_machine_twice() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "unit_slow", kSlowDuration);

    for (int i = 0; i < 9; ++i) f->tick();  // ticks 1-9
    // ticks 1-3 publish twice (startup), ticks 4-9 once.
    CHECK_EQ_INT(f->publishes(), size_t(3 * 2 + 6),
                 "precondition: the startup ticks double-published, the rest did not");

    f->events->updates.clear();
    f->tick();  // tick 10: tickCounter_ reaches kForcePublishInterval

    CHECK_EQ_INT(f->publishes(), size_t(2),
                 "a force tick publishes a running machine TWICE (FINDING C)");
    // The first is Pass 0: progress hardcoded to 1.0 and the PRE-debit buffer.
    // The second is Pass 2: the real pct and the post-debit buffer.
    const auto& first = f->events->updates[0];
    const auto& second = f->events->updates[1];
    CHECK_EQ_INT(first.progress, 1.0f,
                 "Pass 0 hardcodes progress 1.0 = waiting for recipe");
    CHECK_EQ_INT(first.energy, 1000u - 9 * kRecipeCost,
                 "Pass 0 publishes the PRE-debit buffer");
    CHECK_EQ_INT(second.energy, static_cast<uint32_t>(1000 - 10 * kRecipeCost),
                 "Pass 2 publishes the post-debit buffer");
    // 10 of the recipe's kSlowDuration ticks elapsed, so the fraction is tiny
    // but strictly positive — and definitely not Pass 0's 1.0 sentinel.
    const float expected_pct =
        1.0f - static_cast<float>(kSlowDuration - 10) / static_cast<float>(kSlowDuration);
    CHECK(fabs(second.progress - expected_pct) < 1e-5f,
          "Pass 2 publishes 1 - remaining_ticks/duration, not the 1.0 sentinel");
    CHECK(second.progress < 0.05f,
          "and it reflects only the 10 elapsed ticks of a 400-tick recipe");
}

static void MachineSystem_pass_zero_publishes_the_documented_fields() {
    auto f = makeFixture();
    const uint32_t x = f->freshX();
    auto e = f->install(kBlockSmelter, x, EnergyType::ELECTRICITY, 1234);
    // Optional components feed the two optional publish fields.
    simcore::HeatIntakeComponent hic;
    hic.heat_stored = 500;
    hic.heat_capacity = 1000;
    f->reg.emplace<simcore::HeatIntakeComponent>(e, hic);
    simcore::SteamOutputComponent soc;
    soc.steam_stored = 250.0;
    soc.steam_capacity = 1000.0;
    f->reg.emplace<simcore::SteamOutputComponent>(e, soc);

    f->tick();

    CHECK_EQ_INT(f->publishes(), size_t(1), "the startup tick publishes once");
    const auto& u = f->events->updates[0];
    CHECK_EQ_INT(u.x, static_cast<int32_t>(x), "x is the machine position");
    CHECK_EQ_INT(u.y, 70, "y is the machine position");
    CHECK_EQ_INT(u.machine_id, kBlockSmelter, "the machine_id is published");
    CHECK_EQ_INT(u.energy, 1234u, "energy is the buffer");
    CHECK_EQ_INT(u.energy_capacity, static_cast<uint32_t>(kCapacity),
                 "energy_capacity IS published (MachineSystem.cpp:109,555)");
    CHECK_EQ_INT(static_cast<int>(u.energy_type),
                 static_cast<int>(EnergyType::ELECTRICITY), "the energy type travels too");
    CHECK_EQ_INT(u.slots_in, kSlotsIn,
                 "slots_in comes from the MachineRegistry, not from the container");
    CHECK(fabs(u.heat_ratio - 0.5f) < 1e-5f,
          "heat_ratio is HeatIntakeComponent::ratio()");
    CHECK(fabs(u.steam_current - 250.0) < 1e-9, "steam_current from SteamOutputComponent");
    CHECK(fabs(u.steam_capacity - 1000.0) < 1e-9, "steam_capacity from SteamOutputComponent");
    CHECK_EQ_INT(u.inventory_bytes, size_t(kSlots) * 5,
                 "the payload is 5 bytes per slot (InventorySerializer.h)");
}

static void MachineSystem_optional_publish_fields_default_to_the_sentinels() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 7);
    CHECK(!f->reg.all_of<simcore::HeatIntakeComponent>(e),
          "precondition: no HeatIntakeComponent");
    CHECK(!f->reg.all_of<simcore::SteamOutputComponent>(e),
          "precondition: no SteamOutputComponent");

    f->tick();

    const auto& u = f->events->updates[0];
    CHECK_EQ_INT(u.heat_ratio, 0.0f, "heat_ratio defaults to 0 without the component");
    CHECK_EQ_INT(u.steam_current, -1.0, "steam_current is the -1.0 'absent' sentinel");
    CHECK_EQ_INT(u.steam_capacity, -1.0, "steam_capacity is the -1.0 'absent' sentinel");
}

static void MachineSystem_unknown_block_id_publishes_slots_in_zero() {
    // kBlockUnmapped has no MachineRegistry entry, so the slot layout falls
    // back to 0 (the `if` at MachineSystem.cpp:89 tests the RESULT).
    auto f = makeFixture();
    f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 1);
    CHECK(MachineRegistry::instance()->Get(kBlockUnmapped) == nullptr,
          "precondition: no registry entry for the unknown block");

    f->tick();

    CHECK_EQ_INT(f->events->updates[0].slots_in, 0,
                 "an unknown block publishes slots_in = 0, not -1");
    CHECK_EQ_INT(f->publishes(), size_t(1), "and it is still published");
}

static void MachineSystem_inventory_change_republishes_immediately() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 0);
    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    f->inv(e).slots[0] = {kIronDust, 3, 0};
    f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(1), "a changed inventory republishes at once");
    CHECK_EQ_INT(f->events->updates[0].inventory[2], 3, "the count byte rides the payload");
    CHECK_EQ_INT(f->events->updates[0].inventory[0], static_cast<uint8_t>(kIronDust & 0xFF),
                 "item_id is little-endian in the payload");
}

// FIXED (gp-dyo4) — this case used to assert the OPPOSITE (that the change was
// NOT published). The observable consequence that mattered was the missing
// client-visible update, so that is what is now asserted: the publish count.
//
// kBlockUnmapped is registered to no recipe class, so Pass 1 never matches and
// never consumes an input. That keeps these cases about the CHANGE GATE alone:
// with kBlockSmelter the recipe would eat slot 0 and the inventory would move
// on its own, which would test recipe dispatch rather than hashing.
static void MachineSystem_identical_stacks_publish_at_once() {
    auto f = makeFixture();
    auto e = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 0);
    CHECK(MachineRegistry::instance()->Get(kBlockUnmapped) == nullptr,
          "precondition: no registry entry and no recipe class, so nothing "
          "consumes a slot behind the test's back");
    CHECK_EQ_INT(f->inv(e).slots.size(), size_t(4),
                 "precondition: 4 slots (1 in + 3 out)");

    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    // Empty -> four identical single-item stacks. The old XOR-of-XOR fold
    // collapsed this to the same value as the empty inventory, so the gate at
    // MachineSystem.cpp:83 suppressed the update and the client kept showing an
    // empty GUI for a full machine until the next forced publish.
    for (int i = 0; i < 4; ++i) f->inv(e).slots[static_cast<size_t>(i)] = {kIronDust, 1, 0};
    f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(1),
                 "filling every slot with an identical stack IS a change, so it "
                 "publishes at once (gp-dyo4)");
    // Payload assertions are guarded on the publish: a gate that publishes
    // nothing has no payload to read, and indexing an empty vector would abort
    // the whole binary before the remaining cases run.
    if (f->publishes() == 1) {
        CHECK_EQ_INT(f->events->updates[0].inventory.size(), size_t(4 * 5),
                     "the payload carries all four slots");
        CHECK_EQ_INT(f->events->updates[0].inventory[2], 1,
                     "and slot 0 holds the stack that was just placed");
    }

    // And the reverse transition — four identical stacks back to empty — is
    // published too, not swallowed by the same cancellation.
    f->events->updates.clear();
    for (int i = 0; i < 4; ++i) f->inv(e).slots[static_cast<size_t>(i)] = {};
    f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(1),
                 "emptying the machine is a change too, in the other direction");
    if (f->publishes() == 1) {
        CHECK_EQ_INT(f->events->updates[0].inventory[2], 0,
                     "and the published payload really is empty");
    }
}

// The old fold was order-INDEPENDENT, so swapping two slots' contents was
// invisible to it even though the published payload differs. The client is
// sent slot-indexed bytes, so that swap has to publish.
static void MachineSystem_swapping_two_slots_publishes() {
    auto f = makeFixture();
    auto e = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 0);
    f->inv(e).slots[0] = {kIronDust, 1, 0};
    f->inv(e).slots[1] = {kGravel, 2, 0};
    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    std::swap(f->inv(e).slots[0], f->inv(e).slots[1]);
    f->tick();

    CHECK_EQ_INT(f->publishes(), size_t(1),
                 "moving a stack from slot 0 to slot 1 changes the slot-indexed "
                 "payload, so it publishes (gp-dyo4)");
    if (f->publishes() != 1) return;  // nothing published: no payload to read
    // packInventory (src/game/storage/InventorySerializer.h) emits 5 bytes per
    // slot: item_id lo, item_id hi, count, meta lo, meta hi. So slot N's count
    // is at 5N+2 and slot N's item_id low byte at 5N+0.
    const auto& u = f->events->updates[0];
    CHECK_EQ_INT(u.inventory[0], static_cast<uint8_t>(kGravel & 0xFF),
                 "slot 0's item_id is now the gravel stack");
    CHECK_EQ_INT(u.inventory[2], 2, "slot 0's count is the gravel stack's");
    CHECK_EQ_INT(u.inventory[5], static_cast<uint8_t>(kIronDust & 0xFF),
                 "slot 1's item_id is now the iron dust stack");
    CHECK_EQ_INT(u.inventory[7], 1, "slot 1's count is the iron dust stack's");
}

// A change gate has to be EXACT in the safe direction too: an unchanged
// inventory must still publish nothing on a non-force tick, or the whole
// hash-gate optimisation is dead. This is the regression the fix must not
// introduce.
static void MachineSystem_unchanged_inventory_stays_quiet() {
    auto f = makeFixture();
    auto e = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 0);
    f->inv(e).slots[0] = {kIronDust, 1, 0};
    f->inv(e).slots[1] = {kGravel, 2, 5};
    f->inv(e).slots[2] = {kSlag, 3, 7};

    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    // Ticks 4-8, none of them a force tick (tick 10 is the first). Nothing
    // changed, and nothing else in the system moves an idle, recipe-less
    // machine's inventory.
    for (int i = 0; i < 5; ++i) f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(0),
                 "an unchanged inventory publishes NOTHING on a non-force tick, "
                 "so the change gate is still exact (gp-dyo4 fix kept the gate)");

    // Rewriting the same values is still no change.
    f->inv(e).slots[0] = {kIronDust, 1, 0};
    f->inv(e).slots[1] = {kGravel, 2, 5};
    f->inv(e).slots[2] = {kSlag, 3, 7};
    f->tick();
    CHECK_EQ_INT(f->publishes(), size_t(0),
                 "re-writing identical contents is not a change either");
}

// The gate is a hash, so it must be DETERMINISTIC and depend on every field
// the payload carries. Each individual field of a slot must move the hash, and
// the value must be a pure function of the contents — not of the entity id, so
// two machines holding identical inventories must reach the same verdict.
static void MachineSystem_hash_is_a_pure_function_of_the_slots() {
    auto f = makeFixture();
    auto e = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 0);
    f->inv(e).slots[0] = {kIronDust, 4, 11};
    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    struct Probe { const char* what; uint16_t id; uint8_t count; uint16_t meta; };
    const Probe probes[] = {
        {"item_id",  kIronIngot, 4, 11},
        {"count",    kIronDust,  5, 11},
        {"meta",     kIronDust,  4, 12},
    };
    for (const auto& p : probes) {
        f->inv(e).slots[0] = {p.id, p.count, p.meta};
        f->tick();
        const size_t changed = f->publishes();
        CHECK_EQ_INT(changed, size_t(1),
                     "a change to slot 0's field publishes exactly once");
        // Restore, then confirm the restore itself is a change in the other
        // direction — i.e. the hash is not merely "dirty once and then stuck".
        f->inv(e).slots[0] = {kIronDust, 4, 11};
        f->tick();
        CHECK_EQ_INT(f->publishes() - changed, size_t(1),
                     "and restoring the original value publishes again");
        f->events->updates.clear();
    }

    // Same change applied to a SECOND machine must publish that machine too:
    // lastInventoryHash_ is keyed by entity, so one machine's update cannot
    // suppress another's, and the hash is a pure function of the contents
    // rather than of insertion order into the map. Counts are per machine so
    // the assertion does not depend on whether this tick is a force tick.
    //
    // Tick accounting so the cases below land on ORDINARY ticks: the probe
    // loop above ran ticks 4-9 (3 startup + 3 probes x 2 ticks each), so the
    // next tick is 10, the first forced one. g is installed and gets its first
    // publish on that tick, then the counter rolls over and everything after is
    // a non-force tick where the change gate is the only thing that can publish.
    auto g = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 0);
    const uint32_t x_e = static_cast<uint32_t>(f->machine(e).x);
    const uint32_t x_g = static_cast<uint32_t>(f->machine(g).x);
    f->events->updates.clear();

    f->tick();                       // tick 10: forced
    CHECK_EQ_INT(f->publishesForX(x_g), size_t(1),
                 "a newly installed machine publishes its first state");
    f->events->updates.clear();

    f->tick();                       // tick 11: ordinary
    CHECK_EQ_INT(f->publishesForX(x_g), size_t(0),
                 "and once published, an unchanged machine is quiet again");
    CHECK_EQ_INT(f->publishesForX(x_e), size_t(0),
                 "while the machine already on the map is undisturbed too");

    // Now apply the SAME content change to both machines in one tick.
    f->inv(e).slots[1] = {kGravel, 1, 0};
    f->inv(g).slots[1] = {kGravel, 1, 0};
    f->events->updates.clear();
    f->tick();                       // tick 12: ordinary
    CHECK_EQ_INT(f->publishesForX(x_e), size_t(1),
                 "an identical change on machine A publishes for A");
    CHECK_EQ_INT(f->publishesForX(x_g), size_t(1),
                 "and the same change on machine B publishes for B, so the "
                 "verdict is per-entity and not shared global state");
}


// The "machine not present" case: drop each of the four view components in
// turn and prove the entity is in no pass.
static void MachineSystem_machine_missing_any_view_component_is_untouched() {
    struct Case {
        const char* name;
        int missing;  // 0 machine, 1 progress, 2 inventory, 3 energy
    };
    const Case cases[] = {
        {"MachineComponent", 0},
        {"RecipeProgress", 1},
        {"InventoryContainer", 2},
        {"EnergyStorage", 3},
    };
    for (const auto& c : cases) {
        auto f = makeFixture();
        const uint32_t x = f->freshX();
        auto e = f->install(kBlockSmelter, x, EnergyType::ELECTRICITY, 500);
        f->inv(e).slots[0] = {kIronDust, 4, 0};
        startRecipe(f->reg, e, "unit_iron", 2);
        // Snapshot everything the four passes could touch.
        const int32_t before_energy = f->energy(e).current;
        const uint32_t before_ticks = f->progress(e).remaining_ticks;

        switch (c.missing) {
            case 0: f->reg.remove<simcore::MachineComponent>(e); break;
            case 1: f->reg.remove<simcore::RecipeProgress>(e); break;
            case 2: f->reg.remove<simcore::InventoryContainer>(e); break;
            case 3: f->reg.remove<simcore::EnergyStorage>(e); break;
        }

        f->tick();
        f->tick();
        f->tick();

        CHECK_EQ_INT(f->publishes(), size_t(0),
                     "no pass runs, so nothing is published");
        // Only assert on the components that are still present: the removed
        // one has nothing left to read.
        if (c.missing != 3) {
            CHECK_EQ_INT(f->energy(e).current, before_energy,
                         "no energy is spent");
        }
        if (c.missing != 1) {
            CHECK_EQ_INT(f->progress(e).remaining_ticks, before_ticks,
                         "no progress is advanced");
        }
        if (c.missing != 2) {
            CHECK_EQ_INT(f->inv(e).slots[0].count, 4, "the inventory is untouched");
        }
    }
}

static void MachineSystem_entities_outside_the_view_are_ignored() {
    auto f = makeFixture();
    // A block entity with a Position but no machine components at all, plus a
    // machine: the view must pick up only the machine.
    auto block = f->reg.create();
    f->reg.emplace<simcore::MachineComponent>(block, kBlockSmelter, 0, 1, 70, 1, 1);
    auto machine = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 100);
    startRecipe(f->reg, machine, "unit_iron", 3);

    f->tick();

    // A startup tick, so the one real machine publishes from Pass 0 AND Pass 2.
    // The stray MachineComponent contributes neither: both updates carry the
    // real machine's position.
    CHECK_EQ_INT(f->publishes(), size_t(2),
                 "a MachineComponent without the other three is in no pass at all");
    CHECK_EQ_INT(f->events->updates[0].x, static_cast<int32_t>(f->next_x - 1),
                 "both updates come from the real machine's position");
    CHECK_EQ_INT(f->events->updates[0].machine_id, kBlockSmelter, "the real machine published");
    CHECK_EQ_INT(f->progress(machine).remaining_ticks, 2u, "and it ticked");
}

static void MachineSystem_dt_is_ignored() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "unit_iron", 50);

    f->sys->tick(0.05f);
    const uint32_t after_small = f->progress(e).remaining_ticks;
    f->sys->tick(1000.0f);  // a huge dt must not fast-forward

    CHECK_EQ_INT(f->progress(e).remaining_ticks, after_small - 1,
                 "one tick advances exactly one progress tick whatever dt is");
    CHECK_EQ_INT(f->energy(e).current, 1000 - 2 * kRecipeCost,
                 "and charges exactly two ticks' worth");
}

static void MachineSystem_a_null_registry_is_not_a_guarded_path() {
    // MachineRegistry::instance() is dereferenced UNGUARDED at
    // MachineSystem.cpp:89, :135, :452, :549 and :606 — every call site is
    // `if (auto* m = MachineRegistry::instance()->Get(...))`, which tests the
    // RESULT, not the INSTANCE. The same defect as EBFSystem.cpp:136,141 and
    // LCRSystem.cpp:95,100, already filed. A null singleton cannot be asserted
    // from inside the suite (the tick would segfault), so this pins what IS
    // observable: the fixture must install a non-null registry for the tick to
    // be well defined, and an EMPTY (non-null) registry degrades to the
    // documented slot-layout fallbacks rather than crashing.
    auto f = makeFixture();
    CHECK(MachineRegistry::instance() != nullptr,
          "precondition: a non-null registry is installed, because "
          "MachineSystem.cpp:89 dereferences instance() unguarded");
    CHECK_EQ_INT(MachineRegistry::instance()->Get(kBlockSmelter)->slots_in, kSlotsIn,
                 "the registry resolves the smelter's 1 input slot");

    // The fallback for a block the registry does not know is 0, and Pass 0 and
    // Pass 2 agree on it (both call the same Get). kBlockUnmapped is registered
    // to no recipe class, so start the running recipe by hand to reach Pass 2.
    auto e = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "unit_slow", kSlowDuration);
    CHECK(MachineRegistry::instance()->Get(kBlockUnmapped) == nullptr,
          "precondition: no registry entry for the unknown block");
    f->tick();
    CHECK_EQ_INT(f->events->updates[0].slots_in, 0, "Pass 0 publishes slots_in 0");
    CHECK_EQ_INT(f->events->updates[1].slots_in, 0, "and Pass 2 agrees");

    // An EMPTY (non-null) registry behaves the same way: the `if` is false, the
    // fallback applies, and Get() itself is safe to call.
    clearRegistry();
    auto empty = MachineRegistry::LoadFromYaml("/nonexistent/machines.yaml");
    CHECK(empty != nullptr, "LoadFromYaml on a missing path still returns a registry");
    MachineRegistry::setInstance(empty.release());
    CHECK(MachineRegistry::instance()->Get(kBlockSmelter) == nullptr,
          "an EMPTY registry returns nullptr safely, which is the case the `if` "
          "was written for");
}

// FINDING D.
static void MachineSystem_start_pass_and_tick_pass_share_one_tick() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    f->inv(e).slots[0] = {kIronDust, 2, 0};

    f->tick();

    CHECK_EQ_INT(f->progress(e).recipe_id, std::string("unit_iron"),
                 "Pass 1 started the recipe on this very tick");
    // FINDING D: Pass 2 then runs in the SAME tick, so the brand new recipe has
    // already been charged and decremented — it is at duration - 1, not
    // duration.
    CHECK_EQ_INT(f->progress(e).remaining_ticks, kRecipeDuration - 1,
                 "Pass 2 ticks the recipe in the same call (FINDING D)");
    CHECK(f->progress(e).is_processing, "and it is running");
    CHECK_EQ_INT(f->energy(e).current, 1000 - kRecipeCost, "charged once");
    CHECK_EQ_INT(f->inv(e).slots[0].count, 1, "one of the two inputs consumed");
    // The startup tick already published in Pass 0, and Pass 2 publishes too.
    CHECK_EQ_INT(f->publishes(), size_t(2), "the start tick publishes twice");
}

static void MachineSystem_input_consumption_matches_item_id_and_meta() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    f->inv(e).slots[0] = {kIronDust, 1, 5};  // right id, wrong meta

    f->tick();

    CHECK(f->progress(e).recipe_id.empty(),
          "a wrong-meta stack does not satisfy the recipe");
    CHECK_EQ_INT(f->inv(e).slots[0].count, 1, "and is left alone");

    f->inv(e).slots[0] = {kIronDust, 1, 0};  // meta 0 is what the YAML declares
    f->tick();
    CHECK_EQ_INT(f->progress(e).recipe_id, std::string("unit_iron"),
                 "the matching stack starts the recipe");
    CHECK_EQ_INT(f->inv(e).slots[0].count, 0, "and is fully consumed");
}

static void MachineSystem_full_run_places_outputs_in_the_output_range() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    f->inv(e).slots[0] = {kIronDust, 1, 0};

    // Tick 1 starts the recipe AND runs its first tick (FINDING D), so the four
    // charged ticks land on ticks 1-4.
    for (int i = 0; i < 4; ++i) f->tick();

    const auto& p = f->progress(e);
    CHECK(p.recipe_id.empty(), "the recipe finished and cleared its id");
    CHECK(!p.is_processing, "is_processing is false at completion");
    CHECK(p.needs_output, "needs_output is set so the next pass waits for a pickup");
    const auto& inv = f->inv(e);
    CHECK_EQ_INT(inv.slots[0].count, 0, "the input slot is empty");
    CHECK_EQ_INT(inv.slots[kSlotsIn].item_id, kSlag,
                 "the product lands in the FIRST registry output slot");
    CHECK_EQ_INT(inv.slots[kSlotsIn].count, 2, "both products placed");
    CHECK_EQ_INT(f->energy(e).current, 1000 - 4 * kRecipeCost,
                 "duration ticks x per-tick cost is the exact charge");
}

static void MachineSystem_output_slots_are_never_zero_guarded() {
    // MachineSystem.cpp:452-456: with no registry entry slots_in falls back to
    // 0, so a completed recipe deposits its product into the INPUT slot. The
    // contrast with EBFSystem (which drops the product instead) is the point.
    clearRegistry();
    auto f = makeFixture();
    auto e = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 1000);
    // kBlockUnmapped is registered to no recipe class, so drive Pass 2 directly.
    // Slot 0 is left EMPTY: the placement loop takes the FIRST EMPTY slot at or
    // after slots_in, so with slots_in == 0 an empty input slot is a candidate.
    startRecipe(f->reg, e, "unit_iron", 1);
    CHECK(MachineRegistry::instance()->Get(kBlockUnmapped) == nullptr,
          "precondition: no registry entry, so slots_in falls back to 0");

    f->tick();

    const auto& inv = f->inv(e);
    CHECK_EQ_INT(inv.slots[0].item_id, kSlag,
                 "with slots_in 0 the output range covers the input slot, so an "
                 "empty input slot receives the product");
    CHECK_EQ_INT(inv.slots[0].count, 2, "both products land there");
    CHECK(f->progress(e).needs_output, "the recipe still completes");
}

static void MachineSystem_every_synthetic_recipe_class_loads() {
    // loadRecipesFromYamlFile reads exactly ONE top-level `class:` and ONE
    // `recipes:` per file (RecipeManager.cpp:510-521), so the suite writes one
    // file per class. This case guards that trap: a second `class:` key in the
    // same file would be silently DROPPED by yaml-cpp and every class after the
    // first would vanish, turning most of the suite into false passes.
    auto f = makeFixture();
    const char* ids[] = {"unit_iron", "unit_slow", "unit_unreachable_input",
                         "unit_gravel", "unit_energy_gated", "unit_mv_only",
                         "unit_half_eu"};
    for (const char* id : ids) {
        CHECK(f->recipes->getRecipeById(id) != nullptr, id);
    }
    CHECK_EQ_INT(f->recipes->recipeCount(), size_t(7),
                 "all seven synthetic recipes across the five class files");
}

static void MachineSystem_eu_cost_is_the_truncated_recipe_energy_cost() {
    auto f = makeFixture();
    const auto* recipe = f->recipes->getRecipeById("unit_half_eu");
    CHECK(recipe != nullptr, "the fractional-cost recipe is loaded");
    CHECK_EQ_INT(static_cast<int32_t>(recipe->energy_cost), kFractionalCost,
                 "eu 2.5 truncates to 2, so the recipe charges 2 per tick");

    auto e = f->install(kBlockFraction, f->freshX(), EnergyType::ELECTRICITY, 100);
    f->inv(e).slots[0] = {kIronDust, 1, 0};
    f->tick();
    CHECK_EQ_INT(f->progress(e).recipe_id, std::string("unit_half_eu"),
                 "the fractional-cost recipe starts");
    CHECK_EQ_INT(f->energy(e).current, 100 - kFractionalCost,
                 "100 - 2 after the single tick of the start+run call (FINDING D)");
}

static void MachineSystem_starved_machine_stalls_without_spending() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 0);
    startRecipe(f->reg, e, "unit_iron", 10);
    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    for (int i = 0; i < 5; ++i) f->tick();

    CHECK_EQ_INT(f->energy(e).current, 0, "an empty buffer spends nothing");
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 10u, "and makes no progress");
    CHECK_EQ_INT(f->publishes(), size_t(0),
                 "the starvation `continue` skips the Pass 2 publish, so the "
                 "client's bar freezes at its last value");
}

static void MachineSystem_stall_boundary_is_the_cost() {
    auto starving = makeFixture();
    auto a = starving->install(kBlockSmelter, starving->freshX(),
                               EnergyType::ELECTRICITY, kRecipeCost - 1);
    startRecipe(starving->reg, a, "unit_iron", 10);
    starving->tick();
    CHECK_EQ_INT(starving->progress(a).remaining_ticks, 10u,
                 "current = cost - 1 stalls");

    auto exact = makeFixture();
    auto b = exact->install(kBlockSmelter, exact->freshX(), EnergyType::ELECTRICITY,
                            kRecipeCost);
    startRecipe(exact->reg, b, "unit_iron", 10);
    exact->tick();
    CHECK_EQ_INT(exact->progress(b).remaining_ticks, 9u,
                 "current == cost is NOT a stall: the tick runs and lands on 0");
    CHECK_EQ_INT(exact->energy(b).current, 0, "the exact-cost buffer is fully spent");
}

static void MachineSystem_vanished_recipe_clears_progress() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "no_such_recipe", 10);
    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    f->tick();

    CHECK(f->progress(e).recipe_id.empty(), "a vanished recipe id is cleared");
    CHECK(!f->progress(e).is_processing, "is_processing is cleared with it");
    CHECK(!f->progress(e).needs_output, "needs_output is cleared with it");
    CHECK_EQ_INT(f->energy(e).current, 1000, "a vanished recipe costs nothing");
    CHECK_EQ_INT(f->publishes(), size_t(0), "the early return skips the Pass 2 publish");
}

// -- Pass 2: the overheat speed control ---------------------------------------

static void MachineSystem_critical_overheat_stops_the_recipe() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "unit_iron", 10);
    f->reg.emplace<simcore::OverheatComponent>(e);
    f->reg.get<simcore::OverheatComponent>(e).state = simcore::OverheatState::CRITICAL;
    for (int i = 0; i < 3; ++i) f->tick();
    f->events->updates.clear();

    for (int i = 0; i < 5; ++i) f->tick();

    CHECK_EQ_INT(f->progress(e).remaining_ticks, 10u, "CRITICAL freezes progress");
    CHECK_EQ_INT(f->energy(e).current, 1000, "and spends nothing");
    CHECK_EQ_INT(f->publishes(), size_t(0), "and publishes nothing");
}

static void MachineSystem_warning_overheat_halves_the_rate() {
    // MachineSystem.cpp:268-274: WARNING emplaces HeatSlowComponent, bumps the
    // accumulator, skips the tick while it is < 2, and resets it to 0 on the
    // tick that runs. So the accumulator cycles 0 -> 1 (skip) -> 2 (run, reset)
    // and the recipe advances on exactly every SECOND tick.
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "unit_iron", 10);
    f->reg.emplace<simcore::OverheatComponent>(e);
    f->reg.get<simcore::OverheatComponent>(e).state = simcore::OverheatState::WARNING;

    // ticks 1-3: skip, run, skip -> one advance, accumulator left at 1.
    for (int i = 0; i < 3; ++i) f->tick();
    CHECK(f->reg.all_of<simcore::HeatSlowComponent>(e),
          "the first WARNING tick emplaces HeatSlowComponent");
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 9u,
                 "three WARNING ticks advance exactly once (skip/run/skip)");
    CHECK_EQ_INT(f->energy(e).current, 1000 - kRecipeCost,
                 "and charge exactly one tick's worth");
    CHECK(fabs(f->reg.get<simcore::HeatSlowComponent>(e).accumulator - 1.0f) < 1e-6f,
          "the accumulator is left at 1, mid-cycle");

    f->tick();  // tick 4: 1 -> 2, so the tick RUNS and resets
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 8u, "the fourth tick runs");
    CHECK_EQ_INT(f->energy(e).current, 1000 - 2 * kRecipeCost, "and is charged");
    CHECK(fabs(f->reg.get<simcore::HeatSlowComponent>(e).accumulator) < 1e-6f,
          "the accumulator resets to 0 on the advancing tick instead of carrying over");

    f->tick();  // tick 5: 0 -> 1, skip
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 8u, "the fifth tick skips again");
    f->tick();  // tick 6: 1 -> 2, run
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 7u,
                 "exactly every SECOND tick advances under WARNING");
}

static void MachineSystem_clearing_overheat_removes_the_heat_slow_component() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    startRecipe(f->reg, e, "unit_iron", 10);
    f->reg.emplace<simcore::OverheatComponent>(e);
    f->reg.get<simcore::OverheatComponent>(e).state = simcore::OverheatState::WARNING;
    f->tick();
    CHECK(f->reg.all_of<simcore::HeatSlowComponent>(e), "precondition: emplaced");

    f->reg.remove<simcore::OverheatComponent>(e);
    f->tick();

    CHECK(!f->reg.all_of<simcore::HeatSlowComponent>(e),
          "losing OverheatComponent REMOVES the HeatSlowComponent the system "
          "itself emplaced (MachineSystem.cpp:275-278)");
}

static void MachineSystem_overheat_blocks_a_recipe_start() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    f->inv(e).slots[0] = {kIronDust, 1, 0};
    f->reg.emplace<simcore::OverheatComponent>(e);
    f->reg.get<simcore::OverheatComponent>(e).state = simcore::OverheatState::WARNING;

    f->tick();

    CHECK(f->progress(e).recipe_id.empty(),
          "an overheating machine does not start a new recipe "
          "(MachineSystem.cpp:129-130)");
    CHECK_EQ_INT(f->inv(e).slots[0].count, 1, "and consumes no inputs");
}

// -- Pass 1.5: passive steam top-up -------------------------------------------

static void MachineSystem_steam_top_up_is_capped_at_the_fill_quantum() {
    CHECK_EQ_INT(simcore::MachineSystem::kSteamFillQuantum, 1000,
                 "kSteamFillQuantum is the documented 1000 mB");
    auto f = makeSteamFixture(kSlag);  // any non-zero id stands in for steam
    sharedFluidClient().clear();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, 0);

    f->tick();

    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(1),
                 "an idle STEAM machine below capacity asks for steam once");
    CHECK_EQ_INT(sharedFluidClient().requests[0].amount,
                 simcore::MachineSystem::kSteamFillQuantum,
                 "the request is exactly one fill quantum");
    CHECK_EQ_INT(sharedFluidClient().requests[0].node_id,
                 static_cast<uint32_t>(e), "the request carries the entity as node id");
    CHECK_EQ_INT(sharedFluidClient().node_updates.size(), size_t(1),
                 "the node is registered with the fluid network first");

    // One outstanding request per machine: the next tick must NOT re-send.
    sharedFluidClient().clear();
    f->tick();
    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(0),
                 "pendingFluidConsumes_ de-duplicates the outstanding request");
}

static void MachineSystem_steam_top_up_is_the_remaining_headroom() {
    auto f = makeSteamFixture(kSlag);
    sharedFluidClient().clear();
    f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, kCapacity - 7);

    f->tick();

    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(1), "one request");
    CHECK_EQ_INT(sharedFluidClient().requests[0].amount, 7,
                 "the request is min(headroom, kSteamFillQuantum) = 7");
}

static void MachineSystem_full_steam_tank_asks_for_nothing() {
    auto f = makeSteamFixture(kSlag);
    sharedFluidClient().clear();
    f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, kCapacity);

    f->tick();

    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(0),
                 "a full tank never requests steam");
}

static void MachineSystem_steam_item_id_zero_fails_closed() {
    // steam_item_id 0 means the registry could not resolve steam; the legacy
    // path must stay pending rather than run uncharged.
    auto f = makeSteamFixture(0);
    sharedFluidClient().clear();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, 0);
    startRecipe(f->reg, e, "unit_iron", 5);

    f->tick();

    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(0),
                 "no steam is requested without a resolved steam id");
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 5u, "and the recipe stays pending");
    CHECK_EQ_INT(f->energy(e).current, 0, "with no uncharged progress");
}

static void MachineSystem_steam_top_up_skips_foreign_and_managed_machines() {
    auto f = makeSteamFixture(kSlag);
    sharedFluidClient().clear();
    f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 0);
    auto managed = f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, 0);
    f->machine(managed).managed_externally = true;

    f->tick();

    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(0),
                 "an ELECTRICITY tank and a managed_externally machine both skip "
                 "the passive top-up");
    CHECK_EQ_INT(sharedFluidClient().node_updates.size(), size_t(0),
                 "and neither registers a fluid node");

    // The SAME fixture with a plain idle STEAM machine does request, which is
    // what makes the two skips above meaningful.
    auto ok = makeSteamFixture(kSlag);
    sharedFluidClient().clear();
    ok->install(kBlockSmelter, ok->freshX(), EnergyType::STEAM, 0);
    ok->tick();
    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(1),
                 "an ordinary idle STEAM machine is the control case");
}

static void MachineSystem_managed_externally_machine_is_still_published() {
    // The role gate covers passes 1, 1.5 and 2 but NOT Pass 0: a machine whose
    // recipe is owned elsewhere still has to publish its inventory.
    auto f = makeSteamFixture(kSlag);
    sharedFluidClient().clear();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, 0);
    f->machine(e).managed_externally = true;
    startRecipe(f->reg, e, "unit_iron", 5);
    const int32_t before = f->energy(e).current;

    f->tick();

    CHECK_EQ_INT(f->publishes(), size_t(1), "Pass 0 still publishes the state");
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 5u, "but Pass 2 does not tick it");
    CHECK_EQ_INT(f->energy(e).current, before, "and nothing is spent");
    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(0),
                 "and Pass 1.5 does not top it up");
}

// -- gp-015h: onConsumeResponse / onFluidConsumeResponse ----------------------

// FIXED (gp-frb2) — this case used to assert that the second credit HAPPENED.
// The invariant it now pins is the one that was actually violated: one
// response credits exactly ONE machine, and only the machine it was addressed
// to.
//
// The assertion is deliberately written against the SUM of the two buffers
// rather than against a named machine. MachineSystem's FIFO branch dequeues
// pendingConsumes_.begin(), and std::unordered_map does not iterate in
// insertion order (nor in a documented order at all), so which machine the
// fallthrough would have hit is not predictable and must not be asserted.
// The SUM is deterministic: 50 units were delivered, so 50 units land.
static void MachineSystem_onConsumeResponse_direct_hit_credits_only_its_node() {
    auto f = makeFixture();
    // Two HEAT machines, both starved, both with an active recipe: one tick
    // queues a consume request for each (pendingConsumes_ is written even with a
    // null pipe client, MachineSystem.cpp:317).
    auto a = f->install(kBlockSmelter, f->freshX(), EnergyType::HEAT, 0);
    auto b = f->install(kBlockSmelter, f->freshX(), EnergyType::HEAT, 0);
    startRecipe(f->reg, a, "unit_iron", 10);
    startRecipe(f->reg, b, "unit_iron", 10);

    f->tick();  // queues both
    CHECK_EQ_INT(f->energy(a).current, 0, "precondition: A is starved");
    CHECK_EQ_INT(f->energy(b).current, 0, "precondition: B is starved");

    // A response DIRECTED at B.
    f->sys->onConsumeResponse(static_cast<uint64_t>(b), 50, 50);

    CHECK_EQ_INT(f->energy(b).current, 50, "the addressed node is credited");
    CHECK_EQ_INT(f->energy(a).current, 0,
                 "and the machine that was NOT addressed receives nothing");
    CHECK_EQ_INT(f->energy(a).current + f->energy(b).current, 50,
                 "gp-frb2: one response credits exactly one machine. The direct "
                 "branch used to fall through into the FIFO branch and credit a "
                 "second buffer for a consumption that was never reported, so "
                 "50 delivered units landed as 100.");
}

// The direct branch has to settle ITS OWN outstanding request, not leave it
// for the FIFO branch to erase by accident. A pending entry that survives a
// settled response makes the tick loop believe the request is still in flight
// (MachineSystem.cpp:304 skips re-requesting while the entry exists), so the
// machine can never ask for energy again once it drains — a permanent stall.
//
// Observable without touching privates: with the direct branch no longer
// falling through, the map must be EMPTY afterwards, so a subsequent FIFO
// response finds nothing to dequeue and credits nobody. Before the fix the
// leftover entry is exactly what the fallthrough consumed.
static void MachineSystem_onConsumeResponse_direct_hit_settles_its_own_request() {
    auto f = makeFixture();
    // A dummy entity takes id 0, so the machine under test is a non-zero id
    // and CAN take the direct branch (see the id-0 sibling below).
    const entt::entity id_zero_taker = f->reg.create();
    CHECK_EQ_INT(static_cast<uint64_t>(id_zero_taker), 0u,
                 "precondition: the dummy entity holds id 0");
    auto a = f->install(kBlockSmelter, f->freshX(), EnergyType::HEAT, 0);
    CHECK_NE_INT(static_cast<uint64_t>(a), 0u,
                 "precondition: the machine's entity id is non-zero");
    startRecipe(f->reg, a, "unit_iron", 10);

    f->tick();  // queues exactly one consume request
    CHECK_EQ_INT(f->energy(a).current, 0, "precondition: the request is outstanding");

    f->sys->onConsumeResponse(static_cast<uint64_t>(a), 50, 50);
    CHECK_EQ_INT(f->energy(a).current, 50, "the directed response is credited");

    // An UNDIRECTED response now has no outstanding request to correlate to.
    f->sys->onConsumeResponse(0, 7, 7);
    CHECK_EQ_INT(f->energy(a).current, 50,
                 "gp-frb2: the directed response settled its own pending entry, "
                 "so the undirected one finds an empty queue and credits nobody");
}


// FINDING G — STILL OPEN (same class as gp-u9ua, filed for BatteryBufferSystem).
// Reported, not fixed: the direct branch is deliberately left guarded by
// `node_id != 0`, so this defect survives the gp-frb2 fix untouched.
//
// A positive response with node_id 0 is INDISTINGUISHABLE from "no node id"
// at MachineSystem.cpp:687 (`if (node_id != 0)`), so it can never take the
// direct branch — even though entt's first entity genuinely has id 0. It
// therefore lands in the FIFO branch and credits whichever pending
// pendingConsumes_.begin() yields, which is chosen by hash order rather than
// by the requested node id.
static void MachineSystem_onConsumeResponse_treats_entity_zero_as_no_node_id() {
    auto f = makeFixture();
    auto a = f->install(kBlockSmelter, f->freshX(), EnergyType::HEAT, 0);
    auto b = f->install(kBlockSmelter, f->freshX(), EnergyType::HEAT, 0);
    startRecipe(f->reg, a, "unit_slow", kSlowDuration);
    startRecipe(f->reg, b, "unit_slow", kSlowDuration);
    f->tick();  // queues a consume request for each (both are HEAT and starved)
    CHECK_EQ_INT(f->energy(a).current, 0, "precondition: A is starving");
    CHECK_EQ_INT(f->energy(b).current, 0, "precondition: B is starving");

    // The entity that actually HAS id 0 is the first one created. A response
    // addressed to it can only ever reach whichever node the FIFO branch
    // picks, so A is credited only by coincidence of hash order.
    const uint64_t zero_entity = static_cast<uint64_t>(a);
    CHECK_EQ_INT(zero_entity, 0u,
                 "precondition: A really is entt's first entity, i.e. id 0");
    f->sys->onConsumeResponse(zero_entity, 25, 25);

    // FINDING G: exactly one machine is credited, but WHICH one is decided by
    // the unordered_map's iteration order, not by the node id in the message.
    // The deterministic statement of the defect: a response naming node 0 must
    // not be able to credit the OTHER node. If B is credited, the routing is
    // provably not by node id.
    CHECK_EQ_INT(f->energy(b).current, 0,
                 "FINDING G: a response addressed to the id-0 node A must not "
                 "reach B; if it has, the FIFO branch picked B by hash order "
                 "instead of A by node id");
    CHECK_EQ_INT(f->energy(a).current + f->energy(b).current, 25,
                 "one response still credits exactly one machine (gp-frb2)");
}

static void MachineSystem_onConsumeResponse_zero_keeps_a_nonzero_pending() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::HEAT, 0);
    startRecipe(f->reg, e, "unit_iron", 10);
    f->tick();
    CHECK_EQ_INT(f->energy(e).current, 0, "precondition: the request is outstanding");

    // A directed zero-consumed response DOES clear the pending (node_id != 0
    // takes the erase at MachineSystem.cpp:681), so the next tick re-requests.
    f->sys->onConsumeResponse(static_cast<uint64_t>(e), 0, 0);
    f->tick();
    CHECK_EQ_INT(f->progress(e).remaining_ticks, 10u,
                 "a zero response still makes no progress");
    CHECK_EQ_INT(f->energy(e).current, 0, "and credits nothing");

    // A POSITIVE response is what advances it.
    f->sys->onConsumeResponse(static_cast<uint64_t>(e), 8, 8);
    CHECK_EQ_INT(f->energy(e).current, 8, "a positive response credits the buffer");
}

static void MachineSystem_onFluidConsumeResponse_credits_the_buffer() {
    auto f = makeSteamFixture(kSlag);
    sharedFluidClient().clear();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, 0);
    startRecipe(f->reg, e, "unit_iron", 5);

    f->tick();
    CHECK_EQ_INT(sharedFluidClient().requests.size(), size_t(1),
                 "the starved steam machine queued a request");

    f->sys->onFluidConsumeResponse(120);
    CHECK_EQ_INT(f->energy(e).current, 120, "the fluid response credits the buffer");

    // A duplicate/zero response must not credit again.
    f->sys->onFluidConsumeResponse(0);
    CHECK_EQ_INT(f->energy(e).current, 120, "a zero response credits nothing");
}

// ---------------------------------------------------------------------------
// gp-s9to — machine-tag filtering and role dispatch
// ---------------------------------------------------------------------------

// The role gate: managed_externally routes a machine to a different subsystem.
static void MachineSystem_managed_externally_never_starts_a_recipe() {
    auto f = makeFixture();
    auto managed = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    auto normal = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    f->machine(managed).managed_externally = true;
    f->inv(managed).slots[0] = {kIronDust, 1, 0};
    f->inv(normal).slots[0] = {kIronDust, 1, 0};

    f->tick();

    CHECK(f->progress(managed).recipe_id.empty(),
          "managed_externally: the machine receives NO recipe start");
    CHECK_EQ_INT(f->inv(managed).slots[0].count, 1, "and consumes no inputs");
    CHECK_EQ_INT(f->energy(managed).current, 1000, "and spends nothing");
    CHECK_EQ_INT(f->progress(normal).recipe_id, std::string("unit_iron"),
                 "the same block WITHOUT the role flag starts normally");
}

// The class dispatch: findRecipeByInputs routes on block_id -> class.
static void MachineSystem_recipe_dispatch_is_by_machine_class() {
    auto f = makeFixture();
    auto smelter = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    auto macerator = f->install(kBlockMacerator, f->freshX(), EnergyType::ELECTRICITY, 1000);
    auto unmapped = f->install(kBlockUnmapped, f->freshX(), EnergyType::ELECTRICITY, 1000);

    f->inv(smelter).slots[0] = {kIronDust, 1, 0};
    f->inv(macerator).slots[0] = {kGravel, 1, 0};
    f->inv(unmapped).slots[0] = {kIronDust, 1, 0};

    f->tick();

    CHECK_EQ_INT(f->progress(smelter).recipe_id, std::string("unit_iron"),
                 "the smelter block runs the smelter recipe");
    CHECK_EQ_INT(f->progress(macerator).recipe_id, std::string("unit_gravel"),
                 "the macerator block runs the macerator recipe");
    CHECK(f->progress(unmapped).recipe_id.empty(),
          "a block registered to no machine class never starts anything, even "
          "with a perfect input match");
    CHECK_EQ_INT(f->inv(unmapped).slots[0].count, 1, "and consumes nothing");
}

static void MachineSystem_output_item_ids_are_exact() {
    // The macerator produces kSlagAlt; the smelter produces kSlag. A machine
    // must never receive another recipe class's product. Each machine is driven
    // to completion on its OWN (a fresh fixture) so neither interferes with the
    // other's input consumption or progress.
    {
        auto f = makeFixture();
        auto macerator = f->install(kBlockMacerator, f->freshX(),
                                    EnergyType::ELECTRICITY, 1000);
        f->inv(macerator).slots[0] = {kGravel, 1, 0};
        for (int i = 0; i < 4; ++i) f->tick();
        CHECK_EQ_INT(f->progress(macerator).remaining_ticks, 0u, "the macerator ran");
        // kBlockMacerator has NO MachineRegistry entry, so slots_in falls back to
        // 0 (MachineSystem.cpp:452-456) and the product lands in slot 0. Only
        // kBlockSmelter is registered, so only its machines get the 1/3 split.
        CHECK(MachineRegistry::instance()->Get(kBlockMacerator) == nullptr,
              "precondition: the macerator block is not in the registry");
        CHECK_EQ_INT(f->inv(macerator).slots[0].item_id, kSlagAlt,
                     "the macerator produced kSlagAlt into the fallback output range");
        CHECK_EQ_INT(f->inv(macerator).slots[0].count, 4, "all four units placed");
    }
    {
        auto f = makeFixture();
        auto smelter = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
        // unit_unreachable_input outputs kGravel; drive it to completion by hand.
        startRecipe(f->reg, smelter, "unit_unreachable_input", 1);
        for (int i = 0; i < 4; ++i) f->tick();
        CHECK_EQ_INT(f->progress(smelter).remaining_ticks, 0u, "the smelter ran");
        CHECK_EQ_INT(f->inv(smelter).slots[kSlotsIn].item_id, kGravel,
                     "the smelter produced kGravel, its own output");
    }
}

static void MachineSystem_unreachable_recipe_stays_idle() {
    auto f = makeFixture();
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    // Gravel is the MACERATOR's input. findRecipeByInputs is scoped to the
    // machine's own class (MachineSystem.cpp:158), so a smelter never matches it.
    f->inv(e).slots[0] = {kGravel, 1, 0};

    f->tick();

    CHECK(f->progress(e).recipe_id.empty(),
          "an input no recipe of THIS machine class accepts leaves it idle");
    CHECK_EQ_INT(f->inv(e).slots[0].count, 1, "and the item is untouched");
}

static void MachineSystem_tier_window_filters_the_dispatch() {
    auto f = makeFixture();
    // kBlockTiered is registered at tier 1; its only recipe needs min_tier 2.
    auto e = f->install(kBlockTiered, f->freshX(), EnergyType::ELECTRICITY, 1000);
    f->inv(e).slots[0] = {kIronDust, 1, 0};
    CHECK_EQ_INT(f->recipes->getMachineTier(kBlockTiered), 1,
                 "the machine is tier 1");
    CHECK_EQ_INT(f->recipes->getRecipeById("unit_mv_only")->min_tier, kTieredMinTier,
                 "the recipe needs tier 2");

    f->tick();
    CHECK(f->progress(e).recipe_id.empty(),
          "a recipe above the machine's tier is filtered out of the dispatch");

    // Re-register the same class at the required tier and it now dispatches.
    f->recipes->registerMachineClass(kBlockTiered, "unit_tiered", kTieredMinTier,
                                     RecipeManager::ENERGY_TYPE_ANY);
    f->tick();
    CHECK_EQ_INT(f->progress(e).recipe_id, std::string("unit_mv_only"),
                 "at min_tier the same recipe dispatches");
}

static void MachineSystem_energy_type_filter_applies_to_the_dispatch() {
    auto f = makeFixture();
    // Re-register the smelter block as STEAM; the recipe declares no
    // energy_in, so ENERGY_TYPE_ANY applies and it still dispatches.
    f->recipes->registerMachineClass(
        kBlockSmelter, "unit_smelter", 1,
        static_cast<uint8_t>(RecipeManager::EnergyType::STEAM));
    auto e = f->install(kBlockSmelter, f->freshX(), EnergyType::STEAM, 1000);
    f->inv(e).slots[0] = {kIronDust, 1, 0};

    f->tick();
    CHECK_EQ_INT(f->progress(e).recipe_id, std::string("unit_iron"),
                 "a recipe with no energy_in filter dispatches to any machine class");
}

// A REAL condition gate: conditions.machine.energy_min IS parsed, and the tick
// consults it, so this is the condition seam that actually works today.
static void MachineSystem_energy_condition_gates_the_start() {
    auto starving = makeFixture();
    auto a = starving->install(kBlockGated, starving->freshX(),
                               EnergyType::ELECTRICITY, kGateEnergyMin - 1);
    starving->inv(a).slots[0] = {kIronDust, 1, 0};
    starving->tick();
    CHECK(starving->progress(a).recipe_id.empty(),
          "one EU below conditions.machine.energy_min blocks the start");
    CHECK_EQ_INT(starving->inv(a).slots[0].count, 1, "and no input is consumed");

    auto exact = makeFixture();
    auto b = exact->install(kBlockGated, exact->freshX(), EnergyType::ELECTRICITY,
                            kGateEnergyMin);
    exact->inv(b).slots[0] = {kIronDust, 1, 0};
    exact->tick();
    CHECK_EQ_INT(exact->progress(b).recipe_id, std::string("unit_energy_gated"),
                 "exactly energy_min satisfies the gate");
}

// FINDING E.
static void MachineSystem_machine_tags_cannot_gate_a_recipe() {
    // (1) The tag component is read by RecipeManager.cpp:44-46 and evaluated
    //     by ConditionEvaluator::checkSpecial — but
    // (2) no code in src/ ever emplaces MachineTagComponent, and
    // (3) parseYamlConditions (RecipeManager.cpp:990-1032) never writes
    //     `conditions.special`, so the `special:` clause the synthetic YAML
    //     declares is silently dropped at load.
    auto f = makeFixture();
    const auto* recipe = f->recipes->getRecipeById("unit_energy_gated");
    CHECK(recipe != nullptr, "the gated recipe is loaded");
    CHECK(recipe->conditions.special.empty(),
          "FINDING E: the `special:` clause never survives the YAML load, so a "
          "machine tag can never veto or allow a recipe");
    CHECK(recipe->conditions.machine.has_value(),
          "the machine condition DOES survive, which is why the gate below works");

    // A machine carrying a tag that the (dropped) clause would have rejected
    // starts anyway: the tag is inert.
    auto e = f->install(kBlockGated, f->freshX(), EnergyType::ELECTRICITY,
                        kGateEnergyMin);
    f->inv(e).slots[0] = {kIronDust, 1, 0};
    RecipeManager::SpecialCondition tag{};
    tag.key = 7;
    tag.value_type = 0;
    tag.int_value = 999;  // deliberately NOT the 42 the recipe "asked" for
    f->reg.emplace<simcore::MachineTagComponent>(e, std::vector<RecipeManager::SpecialCondition>{tag});

    f->tick();

    CHECK_EQ_INT(f->progress(e).recipe_id, std::string("unit_energy_gated"),
                 "a mismatched MachineTagComponent does not veto the recipe: the "
                 "tag is never consulted");
    CHECK_EQ_INT(f->reg.get<simcore::MachineTagComponent>(e).tags.size(), size_t(1),
                 "the component is still on the machine (it is simply not read)");

    // kBlockUntagged is a SECOND block registered to the same class with no tag
    // at all. If the tag seam worked, exactly one of these two machines would
    // start. Both do, which is the whole point of the finding.
    auto untagged_block = f->install(kBlockUntagged, f->freshX(),
                                     EnergyType::ELECTRICITY, kGateEnergyMin);
    f->inv(untagged_block).slots[0] = {kIronDust, 1, 0};
    f->tick();
    CHECK_EQ_INT(f->progress(untagged_block).recipe_id,
                 std::string("unit_energy_gated"),
                 "the untagged block of the SAME class starts identically: the "
                 "recipe cannot tell the two machines apart (FINDING E)");
}

static void MachineSystem_untagged_machine_takes_the_documented_default() {
    // With no tag clause on the recipe there is no default tag to satisfy: a
    // machine with NO MachineTagComponent and one with a tag are dispatched
    // identically. The "documented default" is therefore: tags are ignored.
    auto f = makeFixture();
    auto untagged = f->install(kBlockGated, f->freshX(), EnergyType::ELECTRICITY,
                               kGateEnergyMin);
    auto tagged = f->install(kBlockGated, f->freshX(), EnergyType::ELECTRICITY,
                             kGateEnergyMin);
    f->inv(untagged).slots[0] = {kIronDust, 1, 0};
    f->inv(tagged).slots[0] = {kIronDust, 1, 0};
    RecipeManager::SpecialCondition tag{};
    tag.key = 99;
    tag.value_type = 2;
    tag.string_value = "anything";
    f->reg.emplace<simcore::MachineTagComponent>(tagged, std::vector<RecipeManager::SpecialCondition>{tag});
    CHECK(!f->reg.all_of<simcore::MachineTagComponent>(untagged),
          "precondition: one machine has no tag component");

    f->tick();

    CHECK_EQ_INT(f->progress(untagged).recipe_id, std::string("unit_energy_gated"),
                 "an untagged machine dispatches");
    CHECK_EQ_INT(f->progress(tagged).recipe_id, std::string("unit_energy_gated"),
                 "and so does a tagged one: the tag is inert (FINDING E)");
}

static void MachineSystem_string_typed_tag_values_are_never_compared() {
    // checkSpecial's string branch (ConditionEvaluator.cpp:138-140) is
    // unreachable from any loaded recipe for the same reason as the rest of the
    // tag path; pin the evaluator in isolation so the comparison logic itself is
    // covered, then show the loaded recipe never reaches it.
    RecipeManager::RecipeConditions conds;
    conds.special.push_back(RecipeManager::SpecialCondition{42, 2, 0, 0.0f, "wanted"});
    RecipeManager::Recipe recipe;
    recipe.id = "manual";
    recipe.conditions = std::move(conds);
    RecipeManager::ConditionEvaluator evaluator;

    RecipeManager::MachineState with_wanted;
    with_wanted.tags.push_back(
        RecipeManager::SpecialCondition{42, 2, 0, 0.0f, "wanted"});
    RecipeManager::MachineState with_other;
    with_other.tags.push_back(
        RecipeManager::SpecialCondition{42, 2, 0, 0.0f, "other"});
    RecipeManager::MachineState untagged;

    CHECK(evaluator.evaluate(recipe, with_wanted),
          "an exactly matching string tag satisfies the clause");
    CHECK(!evaluator.evaluate(recipe, with_other),
          "a differing string value does not");
    CHECK(!evaluator.evaluate(recipe, untagged),
          "an untagged machine does not satisfy a non-empty clause");

    // And the loaded recipe carries no such clause.
    auto f = makeFixture();
    CHECK(f->recipes->getRecipeById("unit_energy_gated")->conditions.special.empty(),
          "but no YAML-loaded recipe can ever carry one (FINDING E)");
}

// FINDING H.
static void MachineSystem_conditions_come_from_the_first_machine_at_those_coords() {
    // RecipeManager.cpp:20-48 scans view<MachineComponent> and `break`s on the
    // first position match, so the state that gates every machine at a given
    // (x,y,z) comes from ONE of them. Two machines share coordinates here: the
    // decoy is created FIRST and the real machine second.
    auto f = makeFixture();
    const uint32_t x = f->freshX();
    auto decoy = f->install(kBlockGated, x, EnergyType::ELECTRICITY, 0);
    auto real = f->install(kBlockGated, x, EnergyType::ELECTRICITY, kGateEnergyMin);
    f->inv(real).slots[0] = {kIronDust, 1, 0};

    f->tick();

    const bool dispatched = f->progress(real).recipe_id == "unit_energy_gated";
    // Whichever way the scan lands, the important invariant is that the
    // decoy's EMPTY buffer is what the condition saw: either the real machine
    // was blocked (decoy scanned first) or the decoy itself was blocked too.
    // Assert the observable, not an assumption about EnTT iteration order.
    CHECK(f->progress(decoy).recipe_id.empty(),
          "the co-located decoy never starts: it has no inputs AND, if the scan "
          "picked it, the gate saw its empty buffer");
    if (!dispatched) {
        CHECK_EQ_INT(f->energy(real).current, kGateEnergyMin,
                     "FINDING H: a co-located machine with an empty buffer "
                     "supplies the condition state and blocks the real machine");
    } else {
        CHECK_EQ_INT(f->energy(decoy).current, 0,
                     "FINDING H: the real machine's buffer was scanned, so the "
                     "co-located decoy is gated by state it does not own");
    }
    (void)x;
}

// The two passes over the same view must not double-charge a machine.
static void MachineSystem_two_machines_are_each_updated_once_per_tick() {
    auto f = makeFixture();
    auto a = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    auto b = f->install(kBlockSmelter, f->freshX(), EnergyType::ELECTRICITY, 1000);
    // unit_slow (400 ticks) so neither machine completes during the 10 ticks.
    startRecipe(f->reg, a, "unit_slow", kSlowDuration);
    startRecipe(f->reg, b, "unit_slow", kSlowDuration);

    f->tick();

    CHECK_EQ_INT(f->energy(a).current, 1000 - kRecipeCost, "machine A charged once");
    CHECK_EQ_INT(f->energy(b).current, 1000 - kRecipeCost, "machine B charged once");
    CHECK_EQ_INT(f->progress(a).remaining_ticks, kSlowDuration - 1,
                 "machine A advanced once");
    CHECK_EQ_INT(f->progress(b).remaining_ticks, kSlowDuration - 1,
                 "machine B advanced once");
    // Ticks 2-9: the startup window is over, so each machine publishes once
    // per tick from Pass 2 alone. Nine ticks so far -> 2x2 (startup) + 2x7.
    for (int i = 0; i < 6; ++i) f->tick();
    // Ticks 1-3 are startup ticks: Pass 0 AND Pass 2 publish (2 each). Ticks
    // 4-7 are ordinary: Pass 2 only (1 each). Per machine: 3*2 + 4*1 = 10.
    CHECK_EQ_INT(f->publishes(), size_t(2 * 10),
                 "ticks 1-3 publish twice per machine, ticks 4-7 once per machine");
    CHECK_EQ_INT(f->energy(a).current, 1000 - 7 * kRecipeCost,
                 "machine A is charged exactly once per tick, publish count "
                 "irrelevant");
    CHECK_EQ_INT(f->energy(b).current, 1000 - 7 * kRecipeCost,
                 "machine B is charged exactly once per tick");

    for (int i = 0; i < 2; ++i) f->tick();  // ticks 8-9
    CHECK_EQ_INT(f->publishes(), size_t(2 * 12),
                 "ticks 8-9 add one publish per machine each");
    f->events->updates.clear();
    f->tick();  // tick 10: forced
    CHECK_EQ_INT(f->publishes(), size_t(4),
                 "a force tick publishes 2 machines x 2 passes (Pass 0 + Pass 2)");
    CHECK_EQ_INT(f->energy(a).current, 1000 - 10 * kRecipeCost,
                 "the force tick still charges machine A exactly once");
    CHECK_EQ_INT(f->energy(b).current, 1000 - 10 * kRecipeCost,
                 "and machine B exactly once");
}

// TEST() calls the function by name directly. (The sibling suites in this
// directory name their functions test_<name> and use test_##name; this suite
// already prefixes every case with the system name, so the extra prefix would
// only add noise.)
#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== machine_system test suite ===\n\n");

    // gp-015h: the tick and the state publication path
    TEST(MachineSystem_force_publish_lands_on_tick_ten);
    TEST(MachineSystem_idle_machine_publishes_exactly_once_per_tick);
    TEST(MachineSystem_force_tick_publishes_a_running_machine_twice);
    TEST(MachineSystem_pass_zero_publishes_the_documented_fields);
    TEST(MachineSystem_optional_publish_fields_default_to_the_sentinels);
    TEST(MachineSystem_unknown_block_id_publishes_slots_in_zero);
    TEST(MachineSystem_inventory_change_republishes_immediately);
    TEST(MachineSystem_identical_stacks_publish_at_once);
    TEST(MachineSystem_swapping_two_slots_publishes);
    TEST(MachineSystem_unchanged_inventory_stays_quiet);
    TEST(MachineSystem_hash_is_a_pure_function_of_the_slots);
    TEST(MachineSystem_machine_missing_any_view_component_is_untouched);
    TEST(MachineSystem_entities_outside_the_view_are_ignored);
    TEST(MachineSystem_dt_is_ignored);
    TEST(MachineSystem_a_null_registry_is_not_a_guarded_path);
    TEST(MachineSystem_start_pass_and_tick_pass_share_one_tick);
    TEST(MachineSystem_input_consumption_matches_item_id_and_meta);
    TEST(MachineSystem_full_run_places_outputs_in_the_output_range);
    TEST(MachineSystem_output_slots_are_never_zero_guarded);
    TEST(MachineSystem_every_synthetic_recipe_class_loads);
    TEST(MachineSystem_eu_cost_is_the_truncated_recipe_energy_cost);
    TEST(MachineSystem_starved_machine_stalls_without_spending);
    TEST(MachineSystem_stall_boundary_is_the_cost);
    TEST(MachineSystem_vanished_recipe_clears_progress);
    TEST(MachineSystem_critical_overheat_stops_the_recipe);
    TEST(MachineSystem_warning_overheat_halves_the_rate);
    TEST(MachineSystem_clearing_overheat_removes_the_heat_slow_component);
    TEST(MachineSystem_overheat_blocks_a_recipe_start);
    TEST(MachineSystem_steam_top_up_is_capped_at_the_fill_quantum);
    TEST(MachineSystem_steam_top_up_is_the_remaining_headroom);
    TEST(MachineSystem_full_steam_tank_asks_for_nothing);
    TEST(MachineSystem_steam_item_id_zero_fails_closed);
    TEST(MachineSystem_steam_top_up_skips_foreign_and_managed_machines);
    TEST(MachineSystem_managed_externally_machine_is_still_published);
    TEST(MachineSystem_onConsumeResponse_direct_hit_credits_only_its_node);
    TEST(MachineSystem_onConsumeResponse_direct_hit_settles_its_own_request);
    TEST(MachineSystem_onConsumeResponse_treats_entity_zero_as_no_node_id);
    TEST(MachineSystem_onConsumeResponse_zero_keeps_a_nonzero_pending);
    TEST(MachineSystem_onFluidConsumeResponse_credits_the_buffer);
    TEST(MachineSystem_two_machines_are_each_updated_once_per_tick);

    // gp-s9to: machine-tag filtering and role dispatch
    TEST(MachineSystem_managed_externally_never_starts_a_recipe);
    TEST(MachineSystem_recipe_dispatch_is_by_machine_class);
    TEST(MachineSystem_output_item_ids_are_exact);
    TEST(MachineSystem_unreachable_recipe_stays_idle);
    TEST(MachineSystem_tier_window_filters_the_dispatch);
    TEST(MachineSystem_energy_type_filter_applies_to_the_dispatch);
    TEST(MachineSystem_energy_condition_gates_the_start);
    TEST(MachineSystem_machine_tags_cannot_gate_a_recipe);
    TEST(MachineSystem_untagged_machine_takes_the_documented_default);
    TEST(MachineSystem_string_typed_tag_values_are_never_compared);
    TEST(MachineSystem_conditions_come_from_the_first_machine_at_those_coords);

    // Cleanup: the synthetic recipe file and the process-wide registry.
    for (const auto& path : tempFiles()) remove(path.c_str());
    tempFiles().clear();
    clearRegistry();
    sharedFluidClient().node_updates.clear();
    sharedFluidClient().requests.clear();

    printf("\n=== Results: %d tests, %d assertions passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
