// gp-hmb0 — "a recipe with an unregistered item name silently becomes a FREE
// recipe".
//
// THE BUG
// ───────
// Four steps, each of which I re-verified before writing this test:
//
//   1. ItemRegistry::nameToId returns 0 for an unknown name, after only a
//      spdlog::warn (ItemRegistry.cpp:225-232).
//   2. parseYamlInputItem -> resolveItemId -> resolveItemName -> nameToId, so
//      an unknown NAME lands in InputItem::item_id as 0 (RecipeManager.cpp).
//   3. The load-time guard pushes it anyway:
//          if (item.item_id != 0 || inputs[i]["item"]) recipe.inputs.push_back(item);
//      That condition tests KEY PRESENCE ("was an item: written?"), not
//      resolution ("did the name exist?"), so a typo sails through.
//   4. matches() then skips the requirement outright:
//          if (req.item_id == 0) continue;
//      so the slot demands nothing, the recipe matches an EMPTY container,
//      and craft() places the output. The output is free.
//
// THE TRAP THAT MAKES THE OBVIOUS FIX WRONG
// ───────────────────────────────────────────
// `0:0:0,air` IS a row in src/content/data/registry/items.csv, and
// ItemId::pack("0:0:0") == 0. So id 0 is simultaneously "resolution failed"
// and "a perfectly valid registered item". Any fix that rejects `id == 0`
// therefore breaks `item: air` — and air is the one id that legitimately has
// to keep meaning "empty slot" for the wildcard/`consume: false` behaviour
// the matcher depends on.
//
// That is why the fix is not "reject id 0" but "ask whether the NAME
// resolved" (ItemRegistry::hasName), and why the match-time skip is left in
// place with its meaning documented rather than deleted.
//
// WHAT THIS TEST PINS
// ────────────────────
//   NEGATIVE  — a recipe naming an unregistered item on either side does not
//               load, is not in the catalog, and cannot be crafted. This is
//               the regression that must never come back: the old behaviour
//               was "loads, matches an empty container, hands you the output".
//   POSITIVE  — every legitimate id-0 use keeps working: the literal
//               `air`, packed ids, flat-numeric ids, a name that RESOLVES to
//               0, inputs with no `item:` key at all, pattern cells that must
//               be empty, and the generator/boiler exemption (recipes with no
//               outputs, which must still load). The last one is the
//               regression this fix is most likely to cause, because the
//               no-outputs rule and the new resolution rule both sit in the
//               same function and both return false.
//
// Harness: the project's own CHECK/TEST macros (src/engine/net/test/test.h),
// same as the two sibling targets. GTest is deliberately not used — it is
// absent from conanfile.txt, and a `find_package(GTest QUIET)` guard would
// silently unregister this test on CI rather than fail it.
//
// Determinism: no wall clock, no network, no CWD dependence (DATA_DIR is an
// absolute path injected by CMake). Fixtures go to mkstemp files that are
// removed again.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

// The manager CLASS cannot be imported as `RecipeManager` — that name is the
// enclosing namespace (same constraint the sibling test documents).
using RecipeManager::ItemStack;
using RecipeMgr = RecipeManager::RecipeManager;

namespace {
constexpr uint16_t kAir = 0;                          // 0:0:0, a REAL item
constexpr uint16_t kCobblestone = ItemId::pack("0:0:2");
constexpr uint16_t kSand = ItemId::pack("0:0:3");

// A name guaranteed absent from items.csv. Not a typo of anything real.
constexpr const char *kUnregistered = "definitely_not_a_registered_item_hmb0";

void ensureRegistry() {
  static bool done = false;
  if (done)
    return;
  RecipeManager::ItemRegistry::instance().loadFromCSV(std::string(DATA_DIR) +
                                                      "/registry/items.csv");
  done = true;
}

std::string writeTempYaml(const std::string &content) {
  char tmpl[] = "/tmp/gtnh_recipe_resolution_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0)
    return {};
  ssize_t wr = write(fd, content.data(), content.size());
  (void)wr;
  close(fd);
  return std::string(tmpl);
}

// Load a one-recipe file and hand back the manager. `loadOk` records whether
// loadRecipesFromYamlFile returned true.
//
// NOTE that bool means "at least one recipe in the file parsed"
// (parseYamlRecipes returns `loaded > 0`), NOT "this recipe loaded". So for a
// file whose only recipe is rejected the loader returns FALSE — the recipe
// vanishes with no distinct signal, which is exactly why the negative tests
// assert recipeCount()/getRecipeById rather than trusting the bool.
RecipeMgr *loadOne(const std::string &yaml, bool *loadOk = nullptr) {
  ensureRegistry();
  auto *mgr = new RecipeMgr();
  const std::string path = writeTempYaml(yaml);
  if (path.empty())
    return mgr;
  const bool ok = mgr->loadRecipesFromYamlFile(path);
  if (loadOk)
    *loadOk = ok;
  std::filesystem::remove(path);
  return mgr;
}

std::string recipeWithInput(const std::string &itemField) {
  return "class: macerator\n"
         "recipes:\n"
         "  - name: hmb0_probe\n"
         "    inputs:\n"
         "      - { " + itemField + ", count: 1 }\n"
         "    outputs:\n"
         "      - { item: \"" + std::to_string(kSand) +
         "\", count: 1 }\n"
         "    duration: 200\n";
}

std::string recipeWithOutput(const std::string &itemField) {
  return "class: macerator\n"
         "recipes:\n"
         "  - name: hmb0_probe\n"
         "    inputs:\n"
         "      - { item: \"" + std::to_string(kCobblestone) +
         "\", count: 1 }\n"
         "    outputs:\n"
         "      - { " + itemField + ", count: 1 }\n"
         "    duration: 200\n";
}
} // namespace

// ===========================================================================
// NEGATIVE: an unregistered name on either side must not produce a recipe
// ===========================================================================

// The headline test. Before the fix this recipe LOADED, matched an empty
// container (the id-0 requirement was skipped) and produced sand for nothing.
static void test_unregistered_input_name_does_not_load() {
  bool loadOk = false;
  std::unique_ptr<RecipeMgr> mgr(
      loadOne(recipeWithInput("item: \"" + std::string(kUnregistered) + "\""),
              &loadOk));

  CHECK(mgr->getRecipeById("hmb0_probe") == nullptr,
        "a recipe whose INPUT names an unregistered item is not loaded");
  CHECK_EQ(mgr->recipeCount(), size_t(0), "the manager holds no recipe at all");

  // loadRecipesFromYamlFile returns "did ANY recipe in the file parse"
  // (parseYamlRecipes: `return loaded > 0`), so a file whose only recipe is
  // rejected reports FALSE. Pinned because it is the difference between a
  // loud, obvious failure and a silently missing recipe.
  CHECK(!loadOk,
        "a file whose only recipe was rejected loads nothing at all");

  // And the free-craft consequence must be gone: nothing to find, so nothing
  // can be crafted even by feeding a machine an empty grid.
  mgr->registerMachineClass(61000, "macerator", 0, 255);
  CHECK(mgr->findRecipeByInputs(61000, {}) == nullptr,
        "no recipe matches an empty container — the free-craft path is closed");
}

static void test_unregistered_output_name_does_not_load() {
  bool loadOk = false;
  std::unique_ptr<RecipeMgr> mgr(
      loadOne(recipeWithOutput("item: \"" + std::string(kUnregistered) + "\""),
              &loadOk));

  CHECK(mgr->getRecipeById("hmb0_probe") == nullptr,
        "a recipe whose OUTPUT names an unregistered item is not loaded");
  CHECK_EQ(mgr->recipeCount(), size_t(0), "the manager holds no recipe at all");
  CHECK(!loadOk,
        "a file whose only recipe was rejected loads nothing at all");

  // The output-side consequence: an unresolved output used to reach the client
  // as a stack carrying id 0. With the recipe rejected there is nothing to
  // craft, so the response path has no id-0 stack to emit.
  CHECK(mgr->craft("hmb0_probe", nullptr) == nullptr,
        "crafting a rejected recipe yields no container");
}

static void test_unregistered_name_in_replace_does_not_load() {
  // `replace:` feeds the same id space. An unresolved replace became 0, which
  // consumeInputs reads as "no replacement", so a `consume: false` bucket
  // input would be consumed anyway. Same bug, one line over.
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: hmb0_bucket\n"
      "    inputs:\n"
      "      - { item: \"" + std::to_string(kCobblestone) +
      "\", count: 1, consume: false, replace: \"" + kUnregistered + "\" }\n"
      "    outputs:\n"
      "      - { item: \"" + std::to_string(kSand) + "\", count: 1 }\n"
      "    duration: 200\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));

  CHECK(mgr->getRecipeById("hmb0_bucket") == nullptr,
        "an unresolved `replace:` name rejects the recipe too");
}

static void test_unregistered_pattern_cell_does_not_load() {
  // A pattern cell takes a bare scalar, not a map. An unresolved name there
  // packed to 0, and matches() reads a 0 cell as "this cell must be EMPTY" —
  // so the typo inverted the requirement rather than dropping it.
  //
  // Shaped as 3 rows of 3 with `~` for empty, which is the form the real
  // content uses (crafting_table.yaml:11-13).
  const std::string yaml =
      "class: crafting_table\n"
      "recipes:\n"
      "  - name: hmb0_pattern\n"
      "    pattern:\n"
      "      - [\"" + std::string(kUnregistered) + "\", ~, ~]\n"
      "      - [~, ~, ~]\n"
      "      - [~, ~, ~]\n"
      "    outputs:\n"
      "      - { item: \"" + std::to_string(kSand) + "\", count: 1 }\n"
      "    duration: 200\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));

  CHECK(mgr->getRecipeById("hmb0_pattern") == nullptr,
        "a pattern cell naming an unregistered item rejects the recipe");
}

// One bad recipe must not take its healthy neighbours down with it.
static void test_a_rejected_recipe_does_not_take_its_file_down() {
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: hmb0_good\n"
      "    inputs:\n"
      "      - { item: \"" + std::to_string(kCobblestone) + "\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"" + std::to_string(kSand) + "\", count: 1 }\n"
      "    duration: 200\n"
      "  - name: hmb0_bad\n"
      "    inputs:\n"
      "      - { item: \"" + std::string(kUnregistered) + "\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"" + std::to_string(kSand) + "\", count: 1 }\n"
      "    duration: 200\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));

  CHECK(mgr->getRecipeById("hmb0_good") != nullptr,
        "the valid recipe in the same file still loads");
  CHECK(mgr->getRecipeById("hmb0_bad") == nullptr, "its bad neighbour does not");
  CHECK_EQ(mgr->recipeCount(), size_t(1), "exactly one recipe survives");
}

// The test above is only meaningful if the *cause* is the name, not the
// surrounding shape. Same file, one token changed, must load.
static void test_the_same_recipe_loads_once_the_name_is_known() {
  std::unique_ptr<RecipeMgr> good(
      loadOne(recipeWithInput("item: \"" + std::to_string(kCobblestone) + "\"")));
  std::unique_ptr<RecipeMgr> bad(
      loadOne(recipeWithInput("item: \"" + std::string(kUnregistered) + "\"")));

  CHECK(good->getRecipeById("hmb0_probe") != nullptr,
        "a packed id in the same slot loads");
  CHECK(bad->getRecipeById("hmb0_probe") == nullptr,
        "an unregistered name in that slot does not");
}

// ===========================================================================
// POSITIVE: every legitimate id-0 use must behave exactly as before
// ===========================================================================

// THE regression this whole fix could have caused. `air` resolves to id 0,
// which is precisely the value the naive "reject id == 0" fix would have
// rejected. It must still load, and still resolve to 0.
static void test_air_still_loads_and_still_resolves_to_zero() {
  ensureRegistry();
  std::unique_ptr<RecipeMgr> mgr(loadOne(recipeWithInput("item: air")));

  const RecipeManager::Recipe *r = mgr->getRecipeById("hmb0_probe");
  CHECK(r != nullptr, "a recipe naming the real item `air` still loads");
  if (r) {
    CHECK_EQ(r->inputs.size(), size_t(1), "the air input is kept");
    if (!r->inputs.empty())
      CHECK_EQ(int(r->inputs[0].item_id), 0, "and `air` still resolves to id 0");
  }

  // The registry-side reason the fix is written the way it is.
  CHECK(RecipeManager::ItemRegistry::instance().hasName("air"),
        "hasName(\"air\") is true — so 'id == 0' cannot mean 'unresolved'");
  CHECK(!RecipeManager::ItemRegistry::instance().hasName(kUnregistered),
        "hasName on an absent name is false — the two are distinguishable");
}

static void test_air_as_output_still_loads() {
  std::unique_ptr<RecipeMgr> mgr(loadOne(recipeWithOutput("item: air")));
  CHECK(mgr->getRecipeById("hmb0_probe") != nullptr,
        "air is legal on the output side as well");
}

// A NAME that resolves, alongside a packed id and a flat-numeric id, in the
// same file. These are the three non-rejected forms of `item:`.
static void test_name_packed_and_numeric_ids_all_still_load() {
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: hmb0_by_name\n"
      "    inputs:\n"
      "      - { item: cobblestone, count: 1 }\n"
      "    outputs:\n"
      "      - { item: sand, count: 1 }\n"
      "    duration: 200\n"
      "  - name: hmb0_by_packed\n"
      "    inputs:\n"
      "      - { item: \"0:0:2\", count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"0:0:3\", count: 1 }\n"
      "    duration: 200\n"
      "  - name: hmb0_by_number\n"
      "    inputs:\n"
      "      - { item: 2, count: 1 }\n"
      "    outputs:\n"
      "      - { item: 3, count: 1 }\n"
      "    duration: 200\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));

  CHECK(mgr->getRecipeById("hmb0_by_name") != nullptr,
        "a registered NAME resolves and loads");
  CHECK(mgr->getRecipeById("hmb0_by_packed") != nullptr,
        "a packed id is packed arithmetically and loads");
  CHECK(mgr->getRecipeById("hmb0_by_number") != nullptr,
        "an all-digits id is packed arithmetically and loads");
  CHECK_EQ(mgr->recipeCount(), size_t(3), "all three forms coexist");

  // A packed id naming an id with NO registry row must still load: the
  // literal form is never a lookup, and this fix must not have made it one.
  std::unique_ptr<RecipeMgr> literal(loadOne(recipeWithInput("item: \"0:0:60000\"")));
  CHECK(literal->getRecipeById("hmb0_probe") != nullptr,
        "an unregistered PACKED id is still accepted (never a name lookup)");
}

// The intentionally-empty input slot: an input node with no `item:` key at
// all. It parses to id 0 and matches() skips it — the exact behaviour the
// match-time skip exists to support, and the reason it must not be deleted.
static void test_an_input_with_no_item_key_still_loads_and_still_skips() {
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: hmb0_empty_slot\n"
      "    inputs:\n"
      "      - { count: 1 }\n"
      "    outputs:\n"
      "      - { item: \"" + std::to_string(kSand) + "\", count: 1 }\n"
      "    duration: 200\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));

  const RecipeManager::Recipe *r = mgr->getRecipeById("hmb0_empty_slot");
  CHECK(r != nullptr, "an input with no `item:` key still loads");

  // And it still means "demands nothing" at match time — the wildcard
  // behaviour the id-0 skip provides, preserved deliberately.
  RecipeManager::Recipe probe;
  probe.id = "probe";
  probe.machine_class = "macerator";
  RecipeManager::InputItem empty{};
  empty.item_id = 0;
  empty.count = 1;
  probe.inputs.push_back(empty);
  CHECK(probe.matches({}), "a resolved-and-intentionally-zero input matches an empty container");
}

// The generator/boiler exemption: those classes may have NO outputs at all,
// because they produce energy instead. The new rejection rule sits in the
// same function and also returns false, so this is where a careless
// implementation would delete working content.
static void test_producer_recipes_with_no_outputs_still_load() {
  const std::string yaml =
      "class: generator\n"
      "recipes:\n"
      "  - name: hmb0_gen\n"
      "    inputs:\n"
      "      - { item: \"" + std::to_string(kCobblestone) + "\", count: 1 }\n"
      "    duration: 200\n"
      "    energy_output: 100.0\n"
      "  - name: hmb0_boil\n"
      "    inputs:\n"
      "      - { item: \"" + std::to_string(kCobblestone) + "\", count: 1 }\n"
      "    duration: 200\n"
      "    energy_output: 100.0\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));

  const RecipeManager::Recipe *gen = mgr->getRecipeById("hmb0_gen");
  CHECK(gen != nullptr, "a generator recipe with no outputs still loads");
  if (gen)
    CHECK(gen->outputs.empty(), "and its outputs stay empty — no item demanded");
  CHECK(mgr->getRecipeById("hmb0_boil") != nullptr,
        "a boiler recipe with no outputs still loads");
}

// The other half of the same rule: a NON-producer with no outputs is still
// rejected, exactly as before this change. Proves the exemption is scoped to
// the producer classes and that the output block was not weakened.
static void test_a_non_producer_without_outputs_is_still_rejected() {
  const std::string yaml =
      "class: macerator\n"
      "recipes:\n"
      "  - name: hmb0_no_outputs\n"
      "    inputs:\n"
      "      - { item: \"" + std::to_string(kCobblestone) + "\", count: 1 }\n"
      "    duration: 200\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));
  CHECK(mgr->getRecipeById("hmb0_no_outputs") == nullptr,
        "a macerator recipe with no outputs is still rejected (unchanged rule)");
}

// A pattern recipe whose empty cells must be empty: the other legitimate
// meaning of id 0, and the one the pattern-path check sits next to.
static void test_a_pattern_with_empty_cells_still_loads() {
  const std::string yaml =
      "class: crafting_table\n"
      "recipes:\n"
      "  - name: hmb0_pattern_ok\n"
      "    pattern:\n"
      "      - [\"0:0:2\", ~, ~]\n"
      "      - [~, ~, ~]\n"
      "      - [~, ~, ~]\n"
      "    outputs:\n"
      "      - { item: \"" + std::to_string(kSand) + "\", count: 1 }\n"
      "    duration: 200\n";
  std::unique_ptr<RecipeMgr> mgr(loadOne(yaml));

  const RecipeManager::Recipe *r = mgr->getRecipeById("hmb0_pattern_ok");
  CHECK(r != nullptr, "a pattern with ~ cells still loads");
  if (r) {
    CHECK(r->has_pattern, "and is still positional");
    CHECK_EQ(int(r->pattern[0].item_id), int(kCobblestone), "cell 0 kept its id");
    CHECK_EQ(int(r->pattern[8].item_id), 0, "cell 8 is still the empty marker");
  }
}

// The real content tree, loaded through the real loader: the guarantee that
// this fix does not change what the server actually accepts today.
static void test_the_real_recipe_tree_still_loads() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(std::string(DATA_DIR) + "/recipes");
  CHECK(mgr.recipeCount() > 300,
        "the real content tree still loads its recipes in bulk");
}

#define TEST(name)                                                             \
  do {                                                                         \
    ++g_tests;                                                                 \
    printf("  TEST: %s\n", #name);                                             \
    test_##name();                                                             \
  } while (0)

int main() {
  TEST(unregistered_input_name_does_not_load);
  TEST(unregistered_output_name_does_not_load);
  TEST(unregistered_name_in_replace_does_not_load);
  TEST(unregistered_pattern_cell_does_not_load);
  TEST(a_rejected_recipe_does_not_take_its_file_down);
  TEST(the_same_recipe_loads_once_the_name_is_known);

  TEST(air_still_loads_and_still_resolves_to_zero);
  TEST(air_as_output_still_loads);
  TEST(name_packed_and_numeric_ids_all_still_load);
  TEST(an_input_with_no_item_key_still_loads_and_still_skips);
  TEST(producer_recipes_with_no_outputs_still_load);
  TEST(a_non_producer_without_outputs_is_still_rejected);
  TEST(a_pattern_with_empty_cells_still_loads);
  TEST(the_real_recipe_tree_still_loads);

  printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
