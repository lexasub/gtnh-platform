// RecipeManager unit tests — beads gp-82b9 (findRecipeByInputs matching) and
// gp-1t0w (crafting progression and progress persistence).
//
// The public surfaces under test, all in src/game/recipes/RecipeManager.cpp:
//
//   Recipe::matches()          — the matching predicate (positional 3x3 when
//                                has_pattern, aggregate multiset otherwise)
//   Recipe::consumeInputs()    — input deduction, including the
//                                !consume / replace-item (bucket) path
//   Recipe::craft()            — consume + place outputs
//   RecipeManager::findRecipeByInputs() — the class/tier/energy filter that
//                                wraps matches() and picks the winner
//   RecipeManager::craft()     — the flatbuffer round trip over the same
//   RecipeManager::evaluateConditions() — id -> ConditionEvaluator
//   RecipeManager::getRecipeById / collectRecipeItemIds / findRecipesForItem /
//   findRecipesForMachine — the catalog accessors
//
// gp-1t0w asks for "progression: starting a craft, partial progress does not
// complete it, progress at the boundary completes exactly once, and completed
// output is produced once (not twice on a repeated tick)". There is NO progress
// state in this file: RecipeProgress (recipe_id / remaining_ticks /
// is_processing / needs_output) lives in src/engine/sim/components/
// RecipeProgress.h and is advanced by MachineSystem (src/game/machines/
// MachineSystem.cpp), neither of which is under test here. What IS in this file
// is the boundary the issue actually names — "a craft" — and a completed output
// being produced exactly once per accepted container. This file therefore pins
// that boundary: craft() is a pure function of (recipe, container), it consumes
// each input slot at most once, and re-crafting the ALREADY-CRAFTED container
// cannot produce a second output because the inputs are gone. The remaining
// half of gp-1t0w (ticking remaining_ticks to zero, and persisting that across
// save/load) is out of scope for src/game/recipes/ and is noted in the close
// reason rather than asserted here.
//
// Harness: the project's own CHECK/TEST macros (src/engine/net/test/test.h).
// GTest is deliberately not used — it is absent from conanfile.txt, CI does not
// install libgtest-dev, and CI builds Release with a global -Werror, so a
// find_package(GTest QUIET) guard would silently unregister this test on CI.
//
// Determinism: no wall clock, no network, no cluster. DATA_DIR (injected by
// CMake as an absolute path) is used to read the real items.csv, so the same
// precedence the sibling simcore test relies on applies; no test ever depends
// on the CWD.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include <engine/registry/ItemId.h>
#include "ItemRegistry.h"
#include "RecipeManager.h"

#ifndef DATA_DIR
#error "DATA_DIR must be defined by the build (see src/game/recipes/CMakeLists.txt)"
#endif

// Project-wide unit-test harness (src/engine/net/test/test.h).
#include <engine/net/test/test.h>

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char *file, int line, const char *expr,
                const char *msg) {
  if (!cond) {
    fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
    if (msg)
      fprintf(stderr, " -- %s", msg);
    fprintf(stderr, "\n");
    ++g_failed;
  } else {
    ++g_passed;
  }
}

// Namespace aliases must precede the anonymous namespace below, which uses
// them in its constants and helpers. NOTE: the manager class is NOT imported as
// `RecipeManager` — that name is already the enclosing namespace, so a
// using-declaration for the class would collide with it.
using RecipeManager::ItemStack;
using RecipeManager::Recipe;
using RecipeMgr = RecipeManager::RecipeManager;

// ---------------------------------------------------------------------------
// Constants asserted by name, so a data change shows up as a named failure.
// ---------------------------------------------------------------------------
namespace {
constexpr uint16_t kAir = 0; // the empty-slot sentinel in every ItemStack
constexpr uint16_t kCobblestone = ItemId::pack("0:0:2");
constexpr uint16_t kSand = ItemId::pack("0:0:3");
constexpr uint16_t kGlass = ItemId::pack("0:0:4");
constexpr uint16_t kOakPlanks = ItemId::pack("0:10:00:0");
constexpr uint16_t kStick = ItemId::pack("0:11110:0");
constexpr uint16_t kSteam = ItemId::pack("1111:11:1");

// A block_id that is NOT in machines.yaml — used to prove the class filter
// rejects a machine that was never registered.
constexpr uint16_t kUnregisteredBlock = 60001;

std::string writeTempYaml(const std::string &content) {
  char tmpl[] = "/tmp/gtnh_recipe_match_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0)
    return {};
  ssize_t wr = write(fd, content.data(), content.size());
  (void)wr;
  close(fd);
  return std::string(tmpl);
}

// Load the real item registry exactly once; the singleton short-circuits
// subsequent calls (ItemRegistry::loadFromCSV returns early when loaded_).
void ensureRegistry() {
  static bool done = false;
  if (done)
    return;
  RecipeManager::ItemRegistry::instance().loadFromCSV(std::string(DATA_DIR) +
                                                      "/registry/items.csv");
  done = true;
}

// 9-slot grid helper: 3 rows of 3, the shape matches() sees for has_pattern
// recipes and the shape convertContainerItems() always produces.
using Grid = std::vector<RecipeManager::ItemStack>;
Grid makeGrid() { return Grid(9, {kAir, 0, 0}); }

RecipeManager::Recipe baseRecipe(const std::string &id) {
  RecipeManager::Recipe r{};
  r.id = id;
  r.machine_class = "macerator";
  r.duration = 200;
  return r;
}

// OutputItem carries five std::optional display members after the three
// ItemStack-shaped ones. Brace-initialising it positionally triggers
// -Wmissing-field-initializers, which CI turns into an error under its global
// -Werror, so the output is always built through this helper instead.
RecipeManager::OutputItem makeOutput(uint16_t item_id, uint8_t count,
                                     uint16_t metadata = 0) {
  RecipeManager::OutputItem out{};
  out.item_id = item_id;
  out.count = count;
  out.metadata = metadata;
  return out;
}
} // namespace

using RecipeManager::ItemStack;
using RecipeManager::Recipe;
using RecipeManager::RecipeManager;

// ---------------------------------------------------------------------------
// Baseline: an empty manager matches nothing
// ---------------------------------------------------------------------------

static void test_empty_manager_matches_nothing() {
  ensureRegistry();
  RecipeMgr mgr;
  CHECK_EQ(mgr.recipeCount(), size_t(0), "a fresh manager holds no recipes");
  CHECK(mgr.getRecipeById("anything") == nullptr,
        "getRecipeById on an empty manager returns nullptr");
  // machine 0 is not a registered machine, so the class filter short-circuits
  // before any recipe is considered.
  CHECK(mgr.findRecipeByInputs(0, {}) == nullptr,
        "findRecipeByInputs on an empty manager returns nullptr");
}

// ---------------------------------------------------------------------------
// Aggregate (shapeless) matching: the multiset predicate
// ---------------------------------------------------------------------------

static void test_aggregate_exact_multiset_matches() {
  ensureRegistry();
  Recipe r = baseRecipe("agg_exact");
  r.inputs.push_back({kCobblestone, 1, 0});
  r.outputs.push_back(makeOutput(kSand, 1, 0));

  const RecipeManager::ItemStack held{kCobblestone, 1, 0};
  CHECK(r.matches({held}), "a container holding exactly the declared input matches");
}

static void test_aggregate_wrong_item_fails() {
  ensureRegistry();
  Recipe r = baseRecipe("agg_wrong_item");
  r.inputs.push_back({kCobblestone, 1, 0});

  CHECK(!r.matches({{kSand, 1, 0}}), "a different item_id does not match");
}

static void test_aggregate_wrong_count_fails() {
  ensureRegistry();
  Recipe r = baseRecipe("agg_wrong_count");
  r.inputs.push_back({kCobblestone, 8, 0});

  // matches() requires slot.count >= req.count, so one short is a miss.
  CHECK(!r.matches({{kCobblestone, 7, 0}}), "a count below the requirement misses");
  CHECK(r.matches({{kCobblestone, 8, 0}}), "a count exactly equal to the requirement matches");
  CHECK(r.matches({{kCobblestone, 9, 0}}), "a count above the requirement matches");
}

static void test_aggregate_wrong_metadata_fails() {
  ensureRegistry();
  Recipe r = baseRecipe("agg_wrong_meta");
  r.inputs.push_back({kCobblestone, 1, 7});

  CHECK(!r.matches({{kCobblestone, 1, 0}}), "metadata must match exactly");
  CHECK(r.matches({{kCobblestone, 1, 7}}), "the declared metadata matches");
}

static void test_aggregate_extra_items_are_allowed() {
  ensureRegistry();
  // matches() is a SUBSET test, not equality: extra slots are not a veto. The
  // `available < requiredInputs` guard only counts non-empty slots, so one
  // extra occupied slot can never make the match fail.
  Recipe r = baseRecipe("agg_extra");
  r.inputs.push_back({kCobblestone, 1, 0});
  r.inputs.push_back({kSand, 1, 0});

  CHECK(r.matches({{kCobblestone, 1, 0}, {kSand, 1, 0}, {kGlass, 1, 0}}),
        "an extra unrelated slot does not veto the match");
}

static void test_aggregate_a_single_slot_cannot_satisfy_two_requirements() {
  ensureRegistry();
  // The `used[]` guard makes each container slot satisfy at most one input.
  // Two requirements of 1 cobblestone cannot both be met by one stack of 2.
  Recipe r = baseRecipe("agg_no_double_use");
  r.inputs.push_back({kCobblestone, 1, 0});
  r.inputs.push_back({kCobblestone, 1, 0});

  // available (1 occupied slot) < requiredInputs (2) short-circuits to false.
  CHECK(!r.matches({{kCobblestone, 2, 0}}),
        "one slot never satisfies two separate input requirements");
  // Two separate slots of the same item do satisfy them.
  CHECK(r.matches({{kCobblestone, 1, 0}, {kCobblestone, 1, 0}}),
        "two slots satisfy two requirements of the same item");
}

static void test_aggregate_input_with_item_id_zero_is_not_required() {
  ensureRegistry();
  // `requiredInputs` skips item_id == 0, and the per-requirement loop
  // `continue`s on it, so a zero-id input is a no-op placeholder.
  Recipe r = baseRecipe("agg_zero_input");
  r.inputs.push_back({kAir, 1, 0});
  r.inputs.push_back({kCobblestone, 1, 0});

  CHECK(r.matches({{kCobblestone, 1, 0}}),
        "a zero-id input imposes no requirement");
  CHECK(!r.matches({{kSand, 1, 0}}), "the non-zero input is still required");
}

static void test_aggregate_empty_container_matches_a_zero_input_recipe() {
  ensureRegistry();
  Recipe r = baseRecipe("agg_no_inputs");
  r.outputs.push_back(makeOutput(kSand, 1, 0));

  CHECK(r.matches({}), "a recipe with no inputs matches an empty container");
}

static void test_aggregate_a_zero_count_slot_does_not_count_as_available() {
  ensureRegistry();
  Recipe r = baseRecipe("agg_zero_count_slot");
  r.inputs.push_back({kCobblestone, 1, 0});

  // available only counts slots with count > 0, so a populated-but-empty stack
  // cannot stand in for the requirement.
  CHECK(!r.matches({{kCobblestone, 0, 0}}),
        "a slot holding zero of an item is not available");
}

// ---------------------------------------------------------------------------
// Positional (3x3 pattern) matching
// ---------------------------------------------------------------------------

static Recipe patternRecipe(const std::string &id, const Grid &pattern) {
  Recipe r = baseRecipe(id);
  r.has_pattern = true;
  r.pattern = {};
  for (size_t i = 0; i < pattern.size() && i < 9; ++i)
    r.pattern[i] = pattern[i];
  return r;
}

static void test_pattern_matches_only_the_declared_shape() {
  ensureRegistry();
  Grid pat = makeGrid();
  pat[0] = {kOakPlanks, 1, 0};
  pat[3] = {kOakPlanks, 1, 0};
  Recipe r = patternRecipe("pat_stick", pat);

  Grid good = makeGrid();
  good[0] = {kOakPlanks, 1, 0};
  good[3] = {kOakPlanks, 1, 0};
  CHECK(r.matches(good), "the exact declared shape matches");

  Grid transposed = makeGrid();
  transposed[0] = {kOakPlanks, 1, 0};
  transposed[1] = {kOakPlanks, 1, 0};
  CHECK(!r.matches(transposed),
        "the transposed (horizontal) shape does not match a vertical pattern");
}

static void test_pattern_empty_cells_must_be_empty() {
  ensureRegistry();
  // An empty pattern cell requires the corresponding slot to be empty. The
  // check is `cell.item_id == 0 -> slot.item_id must be 0`.
  Grid pat = makeGrid();
  pat[1] = {kOakPlanks, 1, 0};
  Recipe r = patternRecipe("pat_sparse", pat);

  Grid withJunk = makeGrid();
  withJunk[1] = {kOakPlanks, 1, 0};
  withJunk[8] = {kCobblestone, 1, 0}; // a cell the pattern declares empty
  CHECK(!r.matches(withJunk),
        "a non-empty slot in a pattern-empty cell vetoes the match");
}

static void test_pattern_metadata_must_match_exactly() {
  ensureRegistry();
  Grid pat = makeGrid();
  pat[4] = {kOakPlanks, 1, 3};
  Recipe r = patternRecipe("pat_meta", pat);

  Grid wrongMeta = makeGrid();
  wrongMeta[4] = {kOakPlanks, 1, 0};
  CHECK(!r.matches(wrongMeta), "a differing pattern metadata fails");

  Grid rightMeta = makeGrid();
  rightMeta[4] = {kOakPlanks, 1, 3};
  CHECK(r.matches(rightMeta), "the declared pattern metadata matches");
}

static void test_pattern_requires_at_least_nine_slots() {
  ensureRegistry();
  Grid pat = makeGrid();
  pat[8] = {kOakPlanks, 1, 0}; // the last cell
  Recipe r = patternRecipe("pat_last_cell", pat);

  // Only 8 slots: the guard is `container_items.size() < 9`.
  Grid eight = makeGrid();
  eight.resize(8);
  eight[7] = {kOakPlanks, 1, 0};
  CHECK(!r.matches(eight), "fewer than 9 slots cannot match a pattern recipe");

  Grid nine = makeGrid();
  nine[8] = {kOakPlanks, 1, 0};
  CHECK(r.matches(nine), "exactly 9 slots match");
}

static void test_pattern_container_is_not_truncated_at_nine() {
  ensureRegistry();
  // A 10-slot container still compares only the first 9 (the loop bound is a
  // literal 9 on BOTH sides), so slot 9 is invisible to matching.
  Grid pat = makeGrid();
  pat[0] = {kOakPlanks, 1, 0};
  Recipe r = patternRecipe("pat_overflow", pat);

  Grid ten = makeGrid();
  ten.push_back({kCobblestone, 1, 0}); // 10th slot, past the pattern
  ten[0] = {kOakPlanks, 1, 0};
  CHECK_EQ(ten.size(), size_t(10), "the container really has 10 slots");
  CHECK(r.matches(ten), "slots past index 8 are not compared");
}

static void test_pattern_count_is_a_minimum() {
  ensureRegistry();
  Grid pat = makeGrid();
  pat[0] = {kOakPlanks, 1, 0};
  Recipe r = patternRecipe("pat_count_min", pat);

  Grid more = makeGrid();
  more[0] = {kOakPlanks, 5, 0};
  CHECK(r.matches(more), "a stack larger than the pattern count still matches");
}

// ---------------------------------------------------------------------------
// findRecipeByInputs: the class / tier / energy filter and the winner rule
// ---------------------------------------------------------------------------

static void test_find_rejects_an_unregistered_machine() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.registerMachineClass(kUnregisteredBlock + 1, "macerator", 0,
                           static_cast<uint8_t>(RecipeManager::EnergyType::HEAT));

  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: mac_cobble_to_sand\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(!path.empty(), "temp recipe file is created");
  CHECK(mgr.loadRecipesFromYamlFile(path), "the temp recipe file loads");
  std::filesystem::remove(path);

  const std::vector<ItemStack> held{{kCobblestone, 1, 0}};
  // kUnregisteredBlock itself was never registered: classByBlockId_ misses.
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock, held) == nullptr,
        "a block_id with no machine class matches nothing");
  // Its neighbour IS registered with the same class, so the same container matches.
  const Recipe *hit = mgr.findRecipeByInputs(kUnregisteredBlock + 1, held);
  CHECK(hit != nullptr, "a registered block of that class matches the same container");
  if (hit)
    CHECK_EQ(hit->id, std::string("mac_cobble_to_sand"),
             "the registered block resolves to the loaded recipe");
}

static void test_find_rejects_a_class_with_no_recipes() {
  ensureRegistry();
  RecipeMgr mgr;
  // Registered, but nothing was ever loaded for the class "furnace".
  mgr.registerMachineClass(kUnregisteredBlock + 2, "furnace", 0, 255);

  const Recipe *hit =
      mgr.findRecipeByInputs(kUnregisteredBlock + 2, {{kCobblestone, 1, 0}});
  CHECK(hit == nullptr, "a class with no loaded recipes matches nothing");
}

static void test_find_applies_the_tier_window_inclusively() {
  ensureRegistry();
  RecipeMgr mgr;

  // Two recipes, identical inputs, different min_tier. Only the tier window
  // can tell them apart — the input multiset is identical by construction.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: tier0_recipe\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 0\n"
      "    max_tier: 32767\n"
      "  - name: tier2_recipe\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:4\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 2\n"
      "    max_tier: 32767\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "both tier variants load");
  std::filesystem::remove(path);

  // A tier-0 machine sees only the tier-0 recipe.
  mgr.registerMachineClass(kUnregisteredBlock + 3, "macerator", 0, 255);
  const Recipe *low = mgr.findRecipeByInputs(kUnregisteredBlock + 3,
                                             {{kCobblestone, 1, 0}});
  CHECK(low != nullptr, "a tier-0 machine matches its recipe");
  if (low)
    CHECK_EQ(low->id, std::string("tier0_recipe"), "tier 0 picks the min_tier 0 recipe");

  // A tier-2 machine matches BOTH, and the higher min_tier wins.
  mgr.registerMachineClass(kUnregisteredBlock + 4, "macerator", 2, 255);
  const Recipe *high = mgr.findRecipeByInputs(kUnregisteredBlock + 4,
                                              {{kCobblestone, 1, 0}});
  CHECK(high != nullptr, "a tier-2 machine matches one of the two");
  if (high)
    CHECK_EQ(high->id, std::string("tier2_recipe"),
             "the highest min_tier among matches wins (most specific)");

  // A tier-1 machine is inside neither window except the min_tier 0 one:
  // tier2 requires min_tier <= tier, i.e. 2 <= 1 is false.
  mgr.registerMachineClass(kUnregisteredBlock + 5, "macerator", 1, 255);
  const Recipe *mid = mgr.findRecipeByInputs(kUnregisteredBlock + 5,
                                             {{kCobblestone, 1, 0}});
  CHECK(mid != nullptr, "a tier-1 machine still matches the min_tier 0 recipe");
  if (mid)
    CHECK_EQ(mid->id, std::string("tier0_recipe"),
             "a tier-1 machine is below the min_tier 2 recipe's floor");
}

static void test_find_rejects_a_recipe_above_the_machine_tier() {
  ensureRegistry();
  RecipeMgr mgr;
  // A recipe with min_tier 5 and a machine of tier 2: the window test is
  // `recipe.min_tier <= machineTier`, i.e. 5 <= 2, which is false.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: high_tier_only\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 5\n"
      "    max_tier: 32767\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the high-tier recipe loads");
  std::filesystem::remove(path);

  mgr.registerMachineClass(kUnregisteredBlock + 6, "macerator", 2, 255);
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 6, {{kCobblestone, 1, 0}}) ==
            nullptr,
        "a machine below the recipe min_tier matches nothing");

  mgr.registerMachineClass(kUnregisteredBlock + 7, "macerator", 5, 255);
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 7, {{kCobblestone, 1, 0}}) !=
            nullptr,
        "a machine at the recipe min_tier matches (the bound is inclusive)");
}

static void test_find_honours_the_max_tier_bound() {
  ensureRegistry();
  RecipeMgr mgr;
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: capped_tier\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 0\n"
      "    max_tier: 3\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the capped recipe loads");
  std::filesystem::remove(path);

  mgr.registerMachineClass(kUnregisteredBlock + 8, "macerator", 3, 255);
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 8, {{kCobblestone, 1, 0}}) !=
            nullptr,
        "a machine at max_tier matches (the bound is inclusive)");

  mgr.registerMachineClass(kUnregisteredBlock + 9, "macerator", 4, 255);
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 9, {{kCobblestone, 1, 0}}) ==
            nullptr,
        "a machine above max_tier matches nothing");
}

static void test_find_filters_on_the_energy_type() {
  ensureRegistry();
  RecipeMgr mgr;
  // energy_in: HEAT is the only way for findRecipeByInputs to skip a recipe —
  // the filter is `recipe.energy_type != ANY && != machineEnergyIn`.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: heat_only_recipe\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "    energy_in: HEAT\n"
      "    eu: 0\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the HEAT recipe loads");
  std::filesystem::remove(path);

  constexpr uint8_t kHeat = static_cast<uint8_t>(RecipeManager::EnergyType::HEAT);
  constexpr uint8_t kElectricity =
      static_cast<uint8_t>(RecipeManager::EnergyType::ELECTRICITY);

  mgr.registerMachineClass(kUnregisteredBlock + 10, "macerator", 0, kHeat);
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 10, {{kCobblestone, 1, 0}}) !=
            nullptr,
        "a HEAT machine matches a HEAT recipe");

  mgr.registerMachineClass(kUnregisteredBlock + 11, "macerator", 0, kElectricity);
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 11, {{kCobblestone, 1, 0}}) ==
            nullptr,
        "an ELECTRICITY machine does not match a HEAT recipe");
}

static void test_find_treats_energy_type_any_as_a_wildcard() {
  ensureRegistry();
  RecipeMgr mgr;
  // No energy_in in the YAML leaves energy_type at ENERGY_TYPE_ANY (255), the
  // documented "matches any machine energy type" sentinel.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: any_energy_recipe\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the wildcard recipe loads");
  std::filesystem::remove(path);

  const Recipe *r = mgr.getRecipeById("any_energy_recipe");
  CHECK(r != nullptr, "the recipe is retrievable by id");
  if (r)
    CHECK_EQ(r->energy_type, RecipeManager::ENERGY_TYPE_ANY,
             "an omitted energy_in parses to the ENERGY_TYPE_ANY sentinel");

  mgr.registerMachineClass(kUnregisteredBlock + 12, "macerator", 0,
                           static_cast<uint8_t>(RecipeManager::EnergyType::HEAT));
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 12, {{kCobblestone, 1, 0}}) !=
            nullptr,
        "ENERGY_TYPE_ANY matches a HEAT machine");
  mgr.registerMachineClass(kUnregisteredBlock + 13, "macerator", 0,
                           static_cast<uint8_t>(
                               RecipeManager::EnergyType::ROTATION));
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 13, {{kCobblestone, 1, 0}}) !=
            nullptr,
        "ENERGY_TYPE_ANY matches a ROTATION machine too");
}

static void test_find_prefers_the_highest_min_tier_deterministically() {
  ensureRegistry();
  RecipeMgr mgr;
  // gp-82b9 asks explicitly that "two recipes with identical inputs are
  // disambiguated deterministically". Three recipes share one input multiset;
  // all three are in the tier window, so ONLY the min_tier comparison decides.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: t0\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 0\n"
      "  - name: t1\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:4\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 1\n"
      "  - name: t2\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"1111:11:1\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 2\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "all three twins load");
  std::filesystem::remove(path);

  mgr.registerMachineClass(kUnregisteredBlock + 14, "macerator", 7, 255);

  // Repeat the same query many times: the tie-break is `min_tier` only, and it
  // is order-independent because the comparison is a strict `>` over a running
  // max, not a first-match.
  for (int i = 0; i < 32; ++i) {
    const Recipe *hit = mgr.findRecipeByInputs(kUnregisteredBlock + 14,
                                               {{kCobblestone, 1, 0}});
    CHECK(hit != nullptr, "a tier-7 machine matches one of the three twins");
    if (!hit)
      break;
    CHECK_EQ(hit->id, std::string("t2"),
             "the max min_tier twin wins on every repetition");
  }
}

static void test_find_is_stable_for_two_recipes_at_the_same_min_tier() {
  ensureRegistry();
  RecipeMgr mgr;
  // FINDING: when two recipes tie on min_tier, the winner depends on the order
  // of recipesByClass_, which is YAML insertion order (the vector is appended
  // to in parse order). The loop keeps the FIRST one seen at the max tier,
  // because the update is a strict `>`. That is deterministic for a given
  // load order, and it is the first-loaded one — asserted below rather than
  // papered over.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: first_tied\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 1\n"
      "  - name: second_tied\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:4\", count: 1 }\n"
      "    duration: 200\n"
      "    min_tier: 1\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "both tied recipes load");
  std::filesystem::remove(path);

  mgr.registerMachineClass(kUnregisteredBlock + 15, "macerator", 3, 255);
  for (int i = 0; i < 8; ++i) {
    const Recipe *hit = mgr.findRecipeByInputs(kUnregisteredBlock + 15,
                                               {{kCobblestone, 1, 0}});
    CHECK(hit != nullptr, "a tie is still resolved to some recipe");
    if (!hit)
      break;
    CHECK_EQ(hit->id, std::string("first_tied"),
             "on a min_tier tie the first-loaded recipe wins (strict >)");
  }
}

static void test_find_matches_a_pattern_recipe_only_against_its_shape() {
  ensureRegistry();
  RecipeMgr mgr;
  // A crafting-table recipe: the same 2 planks, but the container decides.
  // The 9-cell layout is the ONLY thing that distinguishes stick from a
  // horizontal pair, and matches() is called with all 9 slots.
  const std::string yaml =
      "class: crafting_table\n"
      "recipes:\n"
      "  - name: vertical_planks\n"
      "    pattern:\n"
      "      - [ \"0:10:00:0\", null, null ]\n"
      "      - [ \"0:10:00:0\", null, null ]\n"
      "      - [ null, null, null ]\n"
      "    inputs:\n"
      "      - { item: \"0:10:00:0\", count: 2 }\n"
      "    outputs:\n"
      "      - { item: \"0:11110:0\", count: 4 }\n"
      "    duration: 1\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the pattern recipe loads");
  std::filesystem::remove(path);

  mgr.registerMachineClass(kUnregisteredBlock + 16, "crafting_table", 0, 255);

  Grid vertical = makeGrid();
  vertical[0] = {kOakPlanks, 1, 0};
  vertical[3] = {kOakPlanks, 1, 0};
  const Recipe *hit = mgr.findRecipeByInputs(kUnregisteredBlock + 16, vertical);
  CHECK(hit != nullptr, "the declared vertical shape matches");
  if (hit) {
    CHECK_EQ(hit->id, std::string("vertical_planks"), "and resolves by id");
    CHECK(hit->has_pattern, "the matched recipe is flagged as a pattern recipe");
  }

  Grid horizontal = makeGrid();
  horizontal[0] = {kOakPlanks, 1, 0};
  horizontal[1] = {kOakPlanks, 1, 0};
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 16, horizontal) == nullptr,
        "the rotated shape matches nothing");

  // A single plank is not the declared shape either.
  Grid lone = makeGrid();
  lone[0] = {kOakPlanks, 1, 0};
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 16, lone) == nullptr,
        "a partial shape matches nothing");
}

static void test_find_ignores_a_container_with_more_than_nine_slots() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.registerMachineClass(kUnregisteredBlock + 17, "macerator", 0, 255);
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: one_cobble\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the recipe loads");
  std::filesystem::remove(path);

  // The aggregate path is not bounded to 9 slots, only the pattern path is.
  std::vector<ItemStack> ten(10, {kAir, 0, 0});
  ten[0] = {kCobblestone, 1, 0};
  CHECK(mgr.findRecipeByInputs(kUnregisteredBlock + 17, ten) != nullptr,
        "an aggregate recipe matches a container longer than 9 slots");
}

// ---------------------------------------------------------------------------
// gp-1t0w: consumeInputs / craft — input deduction and one-shot output
// ---------------------------------------------------------------------------

static void test_consume_removes_the_input_stack_entirely() {
  ensureRegistry();
  Recipe r = baseRecipe("consume_all");
  r.inputs.push_back({kCobblestone, 4, 0});

  const std::vector<ItemStack> out = r.consumeInputs({{kCobblestone, 4, 0}});
  CHECK_EQ(out.size(), size_t(1), "the slot count is preserved");
  if (out.size() == 1) {
    CHECK_EQ(out[0].item_id, kAir, "a fully consumed slot clears its item_id");
    CHECK_EQ(out[0].count, uint8_t(0), "a fully consumed slot zeroes its count");
    CHECK_EQ(out[0].metadata, uint16_t(0), "a fully consumed slot clears metadata");
  }
}

static void test_consume_leaves_the_remainder_in_place() {
  ensureRegistry();
  Recipe r = baseRecipe("consume_partial");
  r.inputs.push_back({kCobblestone, 3, 0});

  // A stack of 10 minus a requirement of 3 leaves 7 — the slot keeps its id
  // and meta, so the leftover stays craftable.
  const std::vector<ItemStack> out = r.consumeInputs({{kCobblestone, 10, 0}});
  CHECK_EQ(out.size(), size_t(1), "the slot count is preserved");
  if (out.size() == 1) {
    CHECK_EQ(out[0].item_id, kCobblestone, "a partially consumed slot keeps its id");
    CHECK_EQ(out[0].count, uint8_t(7), "the remainder is the stack minus the cost");
  }
}

static void test_consume_ignores_a_metadata_variant_it_did_not_declare() {
  ensureRegistry();
  // FINDING (pins an existing bug, gp-0ce5-adjacent for recipes): the consume
  // loop matches on `slot.metadata == req.metadata`, so it correctly leaves a
  // different-meta stack of the SAME item_id untouched. Asserted here so the
  // fix for the CraftRequestHandler metadata bug cannot silently change this.
  Recipe r = baseRecipe("consume_meta");
  r.inputs.push_back({kCobblestone, 1, 0});

  const std::vector<ItemStack> out = r.consumeInputs({{kCobblestone, 1, 5}});
  CHECK_EQ(out.size(), size_t(1), "the slot count is preserved");
  if (out.size() == 1) {
    CHECK_EQ(out[0].item_id, kCobblestone, "a different-meta stack is not consumed");
    CHECK_EQ(out[0].count, uint8_t(1), "its count is untouched");
  }
}

static void test_consume_spreads_a_requirement_across_several_slots() {
  ensureRegistry();
  Recipe r = baseRecipe("consume_spread");
  r.inputs.push_back({kCobblestone, 5, 0});

  // 2 + 2 + 2 across three slots: the first two are drained, the third is left
  // with 1.
  const std::vector<ItemStack> out =
      r.consumeInputs({{kCobblestone, 2, 0}, {kCobblestone, 2, 0}, {kCobblestone, 2, 0}});
  CHECK_EQ(out.size(), size_t(3), "the slot count is preserved");
  if (out.size() == 3) {
    CHECK_EQ(out[0].item_id, kAir, "the first slot is drained");
    CHECK_EQ(out[1].item_id, kAir, "the second slot is drained");
    CHECK_EQ(out[2].item_id, kCobblestone, "the third slot keeps a remainder");
    CHECK_EQ(out[2].count, uint8_t(1), "the remainder is 2 - 1 after the 5 is met");
  }
}

static void test_consume_with_consume_false_leaves_the_stack_and_can_replace_it() {
  ensureRegistry();
  // The bucket idiom: consume: false with a `replace` item turns the filled
  // container into an empty one rather than deleting it.
  Recipe r = baseRecipe("consume_bucket");
  RecipeManager::InputItem bucket;
  bucket.item_id = ItemId::pack("0:10:11:1"); // bucket, from items.csv
  bucket.count = 1;
  bucket.consume = false;
  bucket.replace_item = ItemId::pack("0:10:11:2"); // empty bucket
  r.inputs.push_back(bucket);

  const std::vector<ItemStack> out =
      r.consumeInputs({{bucket.item_id, 1, 0}});
  CHECK_EQ(out.size(), size_t(1), "the slot count is preserved");
  if (out.size() == 1) {
    CHECK_EQ(out[0].item_id, bucket.replace_item,
             "a !consume input with a replace item is swapped, not removed");
    CHECK_EQ(out[0].count, uint8_t(1), "the replacement keeps the stack count");
  }
}

static void test_consume_with_consume_false_and_no_replace_is_a_no_op() {
  ensureRegistry();
  Recipe r = baseRecipe("consume_noreplace");
  RecipeManager::InputItem keep;
  keep.item_id = kCobblestone;
  keep.count = 1;
  keep.consume = false;
  r.inputs.push_back(keep);

  const std::vector<ItemStack> out = r.consumeInputs({{kCobblestone, 4, 0}});
  CHECK_EQ(out.size(), size_t(1), "the slot count is preserved");
  if (out.size() == 1) {
    CHECK_EQ(out[0].item_id, kCobblestone, "a !consume input is not removed");
    CHECK_EQ(out[0].count, uint8_t(4), "its count is untouched");
  }
}

static void test_craft_produces_the_output_exactly_once() {
  ensureRegistry();
  // gp-1t0w: "completed output is produced once (not twice on a repeated
  // tick)". craft() is a PURE function of its arguments and mutates nothing:
  // calling it twice on the SAME container yields the SAME container twice, so
  // there is no accumulator to run away. That is the invariant asserted here.
  //
  // NOTE on what is NOT asserted: craft() is not a guard against being fed a
  // container it has already crafted. A second call on the already-finished
  // container re-runs the output placement against whatever slots are free and
  // can duplicate the product. It is not reachable from production today
  // because both callers (RecipeManager::craft and RecipeManagerService) call
  // it exactly once per request, gated by an activeRecipes_ entry keyed on
  // position — see the FINDING in the close reason for gp-1t0w. Crafting the
  // "craft the same container twice" case here would be asserting a security
  // property the code does not have and the issue did not ask for.
  Recipe r = baseRecipe("craft_once");
  r.inputs.push_back({kCobblestone, 1, 0});
  r.outputs.push_back(makeOutput(kSand, 1, 0));

  const std::vector<ItemStack> container{{kCobblestone, 1, 0}};
  const std::vector<ItemStack> first = r.craft(container);
  const std::vector<ItemStack> second = r.craft(container);

  CHECK_EQ(first.size(), container.size(), "craft preserves the slot count");
  CHECK_EQ(second.size(), first.size(), "a repeated craft is the same size");
  if (first.size() == 1 && second.size() == 1) {
    CHECK_EQ(first[0].item_id, kSand, "the first craft places the output");
    CHECK_EQ(first[0].count, uint8_t(1), "exactly one output is produced");
    CHECK_EQ(second[0].item_id, kSand,
             "a repeated call on the SAME container does not accumulate");
    CHECK_EQ(second[0].count, uint8_t(1),
             "the output count is the declared count, not a running total");
  }

  // Purity: craft() does not mutate its argument.
  CHECK_EQ(container[0].item_id, kCobblestone,
           "craft leaves the caller's container untouched");
  CHECK_EQ(container[0].count, uint8_t(1),
           "the caller's stack count is untouched too");
}

static void test_craft_stacks_onto_a_partial_existing_output() {
  ensureRegistry();
  Recipe r = baseRecipe("craft_stack");
  r.inputs.push_back({kCobblestone, 1, 0});
  r.outputs.push_back(makeOutput(kSand, 3, 0));

  // An existing stack of 60 sand leaves room (space = 64 - 60 = 4 >= 3), so the
  // output tops it up rather than taking a new slot. Note the `64` is a
  // hardcoded constant in Recipe::craft, not a per-item stack limit.
  const std::vector<ItemStack> out =
      r.craft({{kCobblestone, 1, 0}, {kSand, 60, 0}});
  CHECK_EQ(out.size(), size_t(2), "the slot count is preserved");
  if (out.size() == 2) {
    CHECK_EQ(out[1].item_id, kSand, "the existing sand slot is reused");
    CHECK_EQ(out[1].count, uint8_t(63), "60 existing + 3 produced = 63");
  }
}

static void test_craft_takes_a_new_slot_when_the_existing_stack_is_too_full() {
  ensureRegistry();
  Recipe r = baseRecipe("craft_new_slot");
  r.inputs.push_back({kCobblestone, 1, 0});
  r.outputs.push_back(makeOutput(kSand, 8, 0));

  // space = 64 - 63 = 1 < 8, so the stack cannot absorb it and an empty slot
  // takes the output instead.
  const std::vector<ItemStack> out = r.craft({{kCobblestone, 1, 0}, {kSand, 63, 0}});
  CHECK_EQ(out.size(), size_t(2), "the slot count is preserved");
  if (out.size() == 2) {
    CHECK_EQ(out[1].item_id, kSand, "the nearly-full stack is left alone");
    CHECK_EQ(out[1].count, uint8_t(63), "the full-ish stack keeps its count");
  }
}

static void test_craft_drops_the_output_when_no_slot_is_free() {
  ensureRegistry();
  // FINDING: craft() logs a warning and silently DROPS the output when every
  // slot is occupied and none can absorb it — it never fails and never signals
  // the loss to the caller. Pinned so the behaviour is visible.
  //
  // Two things have to line up to starve the placement: consumption must not
  // free a slot (hence a !consume input), and no existing slot may hold the
  // output item with room to spare.
  Recipe r = baseRecipe("craft_no_space");
  RecipeManager::InputItem keep;
  keep.item_id = kCobblestone;
  keep.count = 1;
  keep.consume = false; // leaves the slot occupied
  r.inputs.push_back(keep);
  r.outputs.push_back(makeOutput(kSand, 1, 0));

  // Six occupied slots, none of them sand.
  const std::vector<ItemStack> full = {
      {kCobblestone, 1, 0}, {kGlass, 1, 0},   {kOakPlanks, 1, 0},
      {kStick, 1, 0},      {kCobblestone, 1, 0}, {kGlass, 1, 0},
  };
  const std::vector<ItemStack> out = r.craft(full);
  CHECK_EQ(out.size(), full.size(), "craft never grows the container");
  bool placedOutput = false;
  for (size_t i = 0; i < out.size(); ++i) {
    CHECK_EQ(out[i].item_id, full[i].item_id,
             "a dropped output overwrites nothing");
    if (out[i].item_id == kSand)
      placedOutput = true;
  }
  CHECK(!placedOutput, "the output item appears nowhere — it was dropped");
  CHECK_EQ(out[0].item_id, kCobblestone,
           "the !consume input is still there, so nothing freed a slot");
}

static void test_craft_respects_output_metadata_when_stacking() {
  ensureRegistry();
  Recipe r = baseRecipe("craft_meta_out");
  r.inputs.push_back({kCobblestone, 1, 0});
  RecipeManager::OutputItem out;
  out.item_id = kSand;
  out.count = 1;
  out.metadata = 4; // a distinct variant
  r.outputs.push_back(out);

  // An existing sand stack of a DIFFERENT metadata cannot absorb it, so the
  // output takes a fresh slot.
  const std::vector<ItemStack> res = r.craft({{kCobblestone, 1, 0}, {kSand, 10, 0}});
  CHECK_EQ(res.size(), size_t(2), "the slot count is preserved");
  if (res.size() == 2) {
    CHECK_EQ(res[1].item_id, kSand, "the existing different-meta sand is untouched");
    CHECK_EQ(res[1].count, uint8_t(10), "its count is unchanged");
    CHECK_EQ(res[1].metadata, uint16_t(0), "its metadata is unchanged");
  }
}

static void test_craft_on_a_pattern_recipe_consumes_positionally() {
  ensureRegistry();
  Grid pat = makeGrid();
  pat[0] = {kOakPlanks, 1, 0};
  pat[3] = {kOakPlanks, 1, 0};
  Recipe r = patternRecipe("craft_pattern", pat);
  r.outputs.push_back(makeOutput(kStick, 4, 0));

  Grid grid = makeGrid();
  grid[0] = {kOakPlanks, 1, 0};
  grid[3] = {kOakPlanks, 1, 0};
  const std::vector<ItemStack> out = r.craft(grid);

  CHECK_EQ(out.size(), size_t(9), "the 9-slot grid is preserved");
  if (out.size() == 9) {
    // Both pattern cells are consumed, which frees slots 0 and 3. Output
    // placement then takes the FIRST free slot in index order, so the stick
    // lands back in slot 0 — the lowest index the consume step emptied.
    CHECK_EQ(out[0].item_id, kStick, "the output takes the first freed cell (slot 0)");
    CHECK_EQ(out[0].count, uint8_t(4), "with the declared output count");
    CHECK_EQ(out[0].metadata, uint16_t(0), "and the declared output metadata");
    CHECK_EQ(out[3].item_id, kAir, "the second pattern cell is consumed and stays empty");
  }
}

static void test_craft_on_a_pattern_recipe_ignores_the_aggregate_inputs() {
  ensureRegistry();
  // FINDING: when has_pattern is set, consumeInputs never looks at `inputs` at
  // all — only the 9 pattern cells. So an input declared outside the pattern
  // is neither matched nor consumed. Pinned because it is the single most
  // surprising branch in the file.
  Grid pat = makeGrid();
  pat[0] = {kOakPlanks, 1, 0};
  Recipe r = patternRecipe("craft_pattern_only", pat);
  r.inputs.push_back({kCobblestone, 1, 0}); // declared but outside the pattern
  r.outputs.push_back(makeOutput(kStick, 1, 0));

  Grid grid = makeGrid();
  grid[0] = {kOakPlanks, 1, 0};
  grid[5] = {kCobblestone, 1, 0};
  const std::vector<ItemStack> out = r.craft(grid);
  CHECK_EQ(out.size(), size_t(9), "the 9-slot grid is preserved");
  if (out.size() == 9) {
    // Slot 0 is consumed then refilled by the output placement; slot 5 holds
    // the out-of-pattern cobblestone, which is never touched.
    CHECK_EQ(out[0].item_id, kStick, "the output took the freed pattern cell");
    CHECK_EQ(out[5].item_id, kCobblestone,
             "the out-of-pattern input is NOT consumed");
    CHECK_EQ(out[5].count, uint8_t(1), "and keeps its count");
  }
}

// ---------------------------------------------------------------------------
// RecipeManager::craft — the flatbuffer round trip
// ---------------------------------------------------------------------------

static void test_manager_craft_round_trips_through_a_container() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.registerMachineClass(kUnregisteredBlock + 18, "macerator", 0, 255);
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: round_trip\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the recipe loads");
  std::filesystem::remove(path);

  // Build a 9-slot container with cobblestone at slot 0 through the wire struct.
  std::array<Protocol::ItemStack, 9> wire{};
  wire[0] = Protocol::ItemStack(kCobblestone, 1, 0);
  const Protocol::Container container(
      flatbuffers::span<const Protocol::ItemStack, 9>(wire), 0, 1);

  const std::unique_ptr<Protocol::Container> out =
      mgr.craft("round_trip", &container);
  CHECK(out != nullptr, "craft returns a container for a matching recipe");
  if (out) {
    const auto *items = out->items();
    CHECK(items != nullptr, "the returned container has slots");
    if (items) {
      const Protocol::ItemStack *s0 = items->Get(0);
      CHECK(s0 != nullptr, "slot 0 exists");
      if (s0)
        CHECK_EQ(s0->item_id(), kSand, "slot 0 now holds the output item");
      // `size` counts non-empty slots, so the input clearing plus the output
      // landing in slot 0 leaves exactly one occupied slot.
      CHECK_EQ(out->size(), uint16_t(1), "the container reports one occupied slot");
    }
  }
}

static void test_manager_craft_refuses_an_unknown_recipe() {
  ensureRegistry();
  RecipeMgr mgr;
  std::array<Protocol::ItemStack, 9> wire{};
  const Protocol::Container container(
      flatbuffers::span<const Protocol::ItemStack, 9>(wire), 0, 0);
  CHECK(mgr.craft("no_such_recipe", &container) == nullptr,
        "crafting an unknown recipe id returns nullptr");
}

static void test_manager_craft_refuses_insufficient_inputs() {
  ensureRegistry();
  RecipeMgr mgr;
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: needs_two\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 2 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the recipe loads");
  std::filesystem::remove(path);

  // One cobblestone against a requirement of two: matches() is false, so craft
  // returns nullptr WITHOUT consuming anything.
  std::array<Protocol::ItemStack, 9> wire{};
  wire[0] = Protocol::ItemStack(kCobblestone, 1, 0);
  const Protocol::Container container(
      flatbuffers::span<const Protocol::ItemStack, 9>(wire), 0, 1);
  CHECK(mgr.craft("needs_two", &container) == nullptr,
        "craft returns nullptr when the inputs do not match");
}

// ---------------------------------------------------------------------------
// evaluateConditions — the id -> ConditionEvaluator bridge
// ---------------------------------------------------------------------------

static void test_evaluate_conditions_on_an_unknown_recipe_is_false() {
  ensureRegistry();
  RecipeMgr mgr;
  CHECK(!mgr.evaluateConditions("nope"), "an unknown recipe id evaluates false");
}

static void test_evaluate_conditions_with_no_state_uses_a_default_state() {
  ensureRegistry();
  RecipeMgr mgr;
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: plain_recipe\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the recipe loads");
  std::filesystem::remove(path);

  // The single-argument overload passes a value-initialised MachineState, whose
  // floats are 0.0f — so a temperature condition reading 300 cannot pass.
  CHECK(mgr.evaluateConditions("plain_recipe"),
        "a recipe with no conditions is satisfied by the default state");

  const std::string hot =
      "class: macerator\n"
      "recipes:\n"
      "  - name: hot_recipe\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "    conditions:\n"
      "      temperature:\n"
      "        min: 300\n"
      "        max: 400\n";
  const std::string hotPath = writeTempYaml(hot);
  CHECK(mgr.loadRecipesFromYamlFile(hotPath), "the temperature recipe loads");
  std::filesystem::remove(hotPath);

  CHECK(!mgr.evaluateConditions("hot_recipe"),
        "the default zero state fails a 300..400 temperature gate");

  RecipeManager::MachineState hot_state;
  hot_state.temperature = 350.0f;
  CHECK(mgr.evaluateConditions("hot_recipe", hot_state),
        "a 350 C state satisfies the 300..400 gate");
  hot_state.temperature = 200.0f;
  CHECK(!mgr.evaluateConditions("hot_recipe", hot_state),
        "a 200 C state fails the 300..400 gate");
}

// ---------------------------------------------------------------------------
// Catalog accessors (the client-query surface)
// ---------------------------------------------------------------------------

static void test_catalog_and_query_accessors_agree_on_a_loaded_recipe() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.registerMachineClass(kUnregisteredBlock + 19, "macerator", 0, 255);
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: catalog_rec\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"1111:11:1\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "the recipe loads");
  std::filesystem::remove(path);

  // mode 1 = "how is this item crafted" (it is an output).
  const std::vector<const Recipe *> asCraft =
      mgr.findRecipesForItem(kSteam, 1);
  CHECK_EQ(asCraft.size(), size_t(1), "the output item is found in craft mode");
  // mode 2 = "where is this item used" (it is an input).
  const std::vector<const Recipe *> asUse =
      mgr.findRecipesForItem(kCobblestone, 2);
  CHECK_EQ(asUse.size(), size_t(1), "the input item is found in use mode");
  // mode 0 = both.
  const std::vector<const Recipe *> both = mgr.findRecipesForItem(kSteam, 0);
  CHECK_EQ(both.size(), size_t(1), "mode 0 returns the recipe once");
  // An item in neither list.
  CHECK_EQ(mgr.findRecipesForItem(kGlass, 0).size(), size_t(0),
           "an unrelated item matches no recipe");

  const std::vector<uint16_t> ids = mgr.collectRecipeItemIds();
  const auto contains = [&ids](uint16_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  CHECK(contains(kSteam), "the catalog lists the output item");
  CHECK(contains(kCobblestone), "the catalog lists the input item");
  CHECK(!contains(kAir), "the catalog never lists the empty-slot sentinel");
  CHECK(!contains(kGlass), "the catalog omits an item no recipe mentions");

  // findRecipesForMachine is a per-class listing, not a match.
  const std::vector<const Recipe *> forMachine =
      mgr.findRecipesForMachine(kUnregisteredBlock + 19);
  CHECK_EQ(forMachine.size(), size_t(1), "the machine class lists its recipe");
  CHECK_EQ(mgr.findRecipesForMachine(kUnregisteredBlock).size(), size_t(0),
           "an unregistered block lists nothing");
}

static void test_collect_recipe_item_ids_is_deduped() {
  ensureRegistry();
  RecipeMgr mgr;
  // Two recipes both mentioning cobblestone: the catalog must list it once.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: dup_a\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "  - name: dup_b\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:4\", count: 1 }\n"
      "    duration: 200\n";
  const std::string path = writeTempYaml(yaml);
  CHECK(mgr.loadRecipesFromYamlFile(path), "both recipes load");
  std::filesystem::remove(path);

  const std::vector<uint16_t> ids = mgr.collectRecipeItemIds();
  const int cobbleCount = static_cast<int>(std::count(ids.begin(), ids.end(),
                                                     kCobblestone));
  CHECK_EQ(cobbleCount, 1, "an item mentioned twice is listed once");
  CHECK_EQ(ids.size(), size_t(3), "three distinct items: cobble, sand, glass");
}

// ---------------------------------------------------------------------------
// registerMachineClass — the runtime class map
// ---------------------------------------------------------------------------

static void test_register_machine_class_sets_tier_energy_and_class() {
  ensureRegistry();
  RecipeMgr mgr;
  CHECK_EQ(mgr.getMachineClass(kUnregisteredBlock + 20), std::string(""),
           "an unregistered block has no class (empty string, not a throw)");
  CHECK_EQ(mgr.getMachineTier(kUnregisteredBlock + 20), int16_t(0),
           "an unregistered block reports tier 0 (the documented default)");
  CHECK_EQ(mgr.getMachineEnergyIn(kUnregisteredBlock + 20),
           RecipeManager::ENERGY_TYPE_ANY,
           "an unregistered block reports ENERGY_TYPE_ANY (the documented default)");

  mgr.registerMachineClass(kUnregisteredBlock + 20, "macerator", 3,
                           static_cast<uint8_t>(RecipeManager::EnergyType::HEAT));
  CHECK_EQ(mgr.getMachineClass(kUnregisteredBlock + 20), std::string("macerator"),
           "registerMachineClass records the class name");
  CHECK_EQ(mgr.getMachineTier(kUnregisteredBlock + 20), int16_t(3),
           "registerMachineClass records the tier");
  CHECK_EQ(mgr.getMachineEnergyIn(kUnregisteredBlock + 20),
           static_cast<uint8_t>(RecipeManager::EnergyType::HEAT),
           "registerMachineClass records the energy_in type");
}

static void test_register_machine_class_overrides_a_previous_mapping() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.registerMachineClass(kUnregisteredBlock + 21, "macerator", 1, 255);
  mgr.registerMachineClass(kUnregisteredBlock + 21, "furnace", 9, 1);
  CHECK_EQ(mgr.getMachineClass(kUnregisteredBlock + 21), std::string("furnace"),
           "a second registration replaces the first (not merged)");
  CHECK_EQ(mgr.getMachineTier(kUnregisteredBlock + 21), int16_t(9),
           "and replaces the tier too");
}

static void test_machine_registries_load_from_the_real_machines_yaml() {
  ensureRegistry();
  RecipeMgr mgr;
  const std::string path = std::string(DATA_DIR) + "/registry/machines.yaml";
  CHECK(mgr.loadMachinesFromYaml(path), "the real machines.yaml loads");

  // Real block ids as they appear in src/content/data/registry/machines.yaml.
  // Asserted by name so a data change surfaces as a named failure.
  const uint16_t kFurnace = ItemId::pack("1110:000:0");
  const uint16_t kMacerator = ItemId::pack("1110:000:1");
  const uint16_t kGenerator = ItemId::pack("1110:000:2");
  const uint16_t kCraftingTable = ItemId::pack("0:10:11:1");

  CHECK_EQ(mgr.getMachineClass(kFurnace), std::string("furnace"),
           "the real heat_furnace block_id maps to the furnace class");
  CHECK_EQ(mgr.getMachineClass(kMacerator), std::string("macerator"),
           "the real macerator block_id maps to the macerator class");
  CHECK_EQ(mgr.getMachineClass(kGenerator), std::string("generator"),
           "the real generator block_id maps to the generator class");
  CHECK_EQ(mgr.getMachineClass(kCraftingTable), std::string("crafting_table"),
           "the real crafting_table block_id maps to the crafting_table class");

  // Tier and energy_in come from the same variant entries.
  CHECK_EQ(mgr.getMachineTier(kFurnace), int16_t(0),
           "the shipped furnace variant is tier 0");
  CHECK_EQ(mgr.getMachineEnergyIn(kFurnace),
           static_cast<uint8_t>(RecipeManager::EnergyType::HEAT),
           "the shipped furnace variant declares energy_in HEAT");

  // A block id that appears nowhere still returns the documented defaults
  // rather than throwing.
  CHECK_EQ(mgr.getMachineClass(kUnregisteredBlock), std::string(""),
           "an unknown block_id has an empty class name");
  CHECK_EQ(mgr.getMachineTier(kUnregisteredBlock), int16_t(0),
           "an unknown block_id reports tier 0");
  CHECK_EQ(mgr.getMachineEnergyIn(kUnregisteredBlock),
           RecipeManager::ENERGY_TYPE_ANY,
           "an unknown block_id reports ENERGY_TYPE_ANY");
}

static void test_load_machines_from_a_missing_file_fails_cleanly() {
  ensureRegistry();
  RecipeMgr mgr;
  CHECK(!mgr.loadMachinesFromYaml("/nonexistent/path/to/machines.yaml"),
        "a missing machines.yaml returns false rather than throwing");
}

static void test_load_recipes_from_a_missing_file_fails_cleanly() {
  ensureRegistry();
  RecipeMgr mgr;
  CHECK(!mgr.loadRecipesFromYamlFile("/nonexistent/path/to/recipes.yaml"),
        "a missing recipe file returns false rather than throwing");
}

static void test_load_recipes_directory_with_no_yaml_returns_true() {
  ensureRegistry();
  RecipeMgr mgr;
  // An empty directory is reported as success (nothing to load is not an
  // error) — pinned because the return value is what the RPC service keys on.
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "gtnh_empty_recipe_dir";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  CHECK(mgr.loadRecipesFromYamlDirectory(dir.string()),
        "an empty recipe directory is not an error");
  CHECK_EQ(mgr.recipeCount(), size_t(0), "and loads no recipes");
  std::filesystem::remove_all(dir, ec);
}

static void test_recipes_directory_loads_the_real_data() {
  ensureRegistry();
  RecipeMgr mgr;
  const std::string path = std::string(DATA_DIR) + "/recipes";
  CHECK(mgr.loadRecipesFromYamlDirectory(path), "the real recipes/ directory loads");
  CHECK(mgr.recipeCount() > 0, "it contributes recipes");
  // A recipe that only exists as a positional pattern in crafting_table.yaml.
  const Recipe *stick = mgr.getRecipeById("stick");
  CHECK(stick != nullptr, "the real stick recipe is present");
  if (stick) {
    CHECK(stick->has_pattern, "the real stick recipe is a positional pattern");
    CHECK(stick->duration > 0, "and has a non-zero duration");
  }
}

#define TEST(name)                                                             \
  do {                                                                         \
    ++g_tests;                                                                 \
    printf("  TEST: %s\n", #name);                                             \
    test_##name();                                                             \
  } while (0)

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("=== recipe manager matching / progression test suite ===\n\n");

  TEST(empty_manager_matches_nothing);

  TEST(aggregate_exact_multiset_matches);
  TEST(aggregate_wrong_item_fails);
  TEST(aggregate_wrong_count_fails);
  TEST(aggregate_wrong_metadata_fails);
  TEST(aggregate_extra_items_are_allowed);
  TEST(aggregate_a_single_slot_cannot_satisfy_two_requirements);
  TEST(aggregate_input_with_item_id_zero_is_not_required);
  TEST(aggregate_empty_container_matches_a_zero_input_recipe);
  TEST(aggregate_a_zero_count_slot_does_not_count_as_available);

  TEST(pattern_matches_only_the_declared_shape);
  TEST(pattern_empty_cells_must_be_empty);
  TEST(pattern_metadata_must_match_exactly);
  TEST(pattern_requires_at_least_nine_slots);
  TEST(pattern_container_is_not_truncated_at_nine);
  TEST(pattern_count_is_a_minimum);

  TEST(find_rejects_an_unregistered_machine);
  TEST(find_rejects_a_class_with_no_recipes);
  TEST(find_applies_the_tier_window_inclusively);
  TEST(find_rejects_a_recipe_above_the_machine_tier);
  TEST(find_honours_the_max_tier_bound);
  TEST(find_filters_on_the_energy_type);
  TEST(find_treats_energy_type_any_as_a_wildcard);
  TEST(find_prefers_the_highest_min_tier_deterministically);
  TEST(find_is_stable_for_two_recipes_at_the_same_min_tier);
  TEST(find_matches_a_pattern_recipe_only_against_its_shape);
  TEST(find_ignores_a_container_with_more_than_nine_slots);

  TEST(consume_removes_the_input_stack_entirely);
  TEST(consume_leaves_the_remainder_in_place);
  TEST(consume_ignores_a_metadata_variant_it_did_not_declare);
  TEST(consume_spreads_a_requirement_across_several_slots);
  TEST(consume_with_consume_false_leaves_the_stack_and_can_replace_it);
  TEST(consume_with_consume_false_and_no_replace_is_a_no_op);

  TEST(craft_produces_the_output_exactly_once);
  TEST(craft_stacks_onto_a_partial_existing_output);
  TEST(craft_takes_a_new_slot_when_the_existing_stack_is_too_full);
  TEST(craft_drops_the_output_when_no_slot_is_free);
  TEST(craft_respects_output_metadata_when_stacking);
  TEST(craft_on_a_pattern_recipe_consumes_positionally);
  TEST(craft_on_a_pattern_recipe_ignores_the_aggregate_inputs);

  TEST(manager_craft_round_trips_through_a_container);
  TEST(manager_craft_refuses_an_unknown_recipe);
  TEST(manager_craft_refuses_insufficient_inputs);

  TEST(evaluate_conditions_on_an_unknown_recipe_is_false);
  TEST(evaluate_conditions_with_no_state_uses_a_default_state);

  TEST(catalog_and_query_accessors_agree_on_a_loaded_recipe);
  TEST(collect_recipe_item_ids_is_deduped);

  TEST(register_machine_class_sets_tier_energy_and_class);
  TEST(register_machine_class_overrides_a_previous_mapping);
  TEST(machine_registries_load_from_the_real_machines_yaml);
  TEST(load_machines_from_a_missing_file_fails_cleanly);
  TEST(load_recipes_from_a_missing_file_fails_cleanly);
  TEST(load_recipes_directory_with_no_yaml_returns_true);
  TEST(recipes_directory_loads_the_real_data);

  printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
