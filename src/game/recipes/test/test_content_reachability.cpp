// Content reachability guard — beads gp-b2pl.
//
// WHY THIS EXISTS
// ───────────────
// The quest graph only exposes 3 blocked quests, so a player walks into a wall
// of silently stranded content with no signal. gp-b2pl is the concrete
// instance: `transformer_lv_mv` (1110:110:2) was registered in items.csv next to
// four sibling transformers that all had recipes, but had no recipe of its own.
// Quest 74 ("Transformer LV-MV") requires exactly that item as a `craft`, so
// the quest was unpassable and nothing in the build said so. The same shape of
// bug strands whole subtrees: an item whose only producer needs something that
// cannot be made is dead, and so is everything downstream of it.
//
// WHAT THIS TEST DOES
// ───────────────────
//   1. Loads recipes through the REAL loader
//      (RecipeManager::loadRecipesFromYamlDirectory against the real
//      ItemRegistry), so a recipe is "accepted" here exactly when the server
//      accepts it — including parseYamlRecipe's rule that a recipe whose class
//      is neither `generator` nor `boiler` and which has no usable `outputs` is
//      REJECTED and produces nothing (RecipeManager.cpp:699-714).
//      Reimplementing that rule inside the test would let the test and the
//      server disagree, which is precisely the bug class this guards.
//   2. Derives the world base from GENERATOR SOURCE, not from a list written
//      by hand:
//        · WorldGenerator.cpp:22-26  — air, stone, grass, dirt, water
//        · TreeGenerator.h:24-25     — oak log, oak leaves
//        · registry/ores.json        — every block id a vein can place
//        · registry/drops.csv        — each rule's RESULT is obtainable;
//                                        a SOURCE that has a rule is NOT.
//          BlockDrops.cpp:79 substitutes the result for the source, so mining
//          stone yields cobblestone and stone never enters an inventory that
//          way. Counting a dropped source as obtainable would make
//          `base:smelting_cobblestone` (cobblestone in, stone out) look like a
//          way to get stone, which it is not.
//   3. Computes the fixpoint: an item is craftable if AT LEAST ONE accepted
//      recipe has every required input already craftable. A positional recipe
//      (has_pattern) is checked against its 3x3 pattern, NOT against the
//      aggregate `inputs` metadata: Recipe::matches matches on the pattern
//      whenever one is present, so the aggregate list is display-only and
//      using it would invent requirements the server never enforces.
//   4. Asserts against a RECORDED BASELINE, not against today's count.
//
// WHY A SUBSET AND NOT AN EXACT COUNT OR A CEILING
// ────────────────────────────────────────────────
// A subset assertion is the only one of the three that holds in both
// directions at once:
//
//   · Exact equality (`unreachable == baseline`) fails the moment content
//     improves. Other work is actively adding ore veins and dust recipes, so a
//     *correct* fix would turn the build red. A guard that punishes progress
//     gets switched off, and a switched-off guard protects nothing.
//   · A bare count ceiling (`unreachable <= N`) is weaker than it looks: one
//     new strand slips through whenever some other strand is fixed in the same
//     change, so it misses the exact regression it was added for, and it cannot
//     tell anyone which item to go fix.
//   · A subset (`unreachable ⊆ baseline`) tolerates every improvement and
//     fails on each individual new unreachable item BY NAME — the only form of
//     the failure that anyone can act on.
//
// The baseline is a snapshot of a known-bad state, not a statement of intent.
// It is a ratchet: it may grow, but growing it is taking on debt, and every
// entry is an item a player currently cannot obtain.
//
// REGENERATING THE BASELINE
// ─────────────────────────
// 1. Build and run this target; it prints every id that is unreachable but not
//    in the baseline.
// 2. If an id is listed, decide whether it is a real regression (fix the
//    content) or a deliberate snapshot update (add the id below).
// 3. Never delete an entry that is still unreachable: that is what turns the
//    guard back into a lie.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <engine/registry/ItemId.h>
#include "ItemRegistry.h"
#include "RecipeManager.h"

#ifndef DATA_DIR
#error "DATA_DIR must be injected by CMake (see src/game/recipes/CMakeLists.txt)"
#endif

// ---------------------------------------------------------------------------
// Harness: the project's own CHECK/TEST macros (src/engine/net/test/test.h).
// GTest is deliberately not used — it is absent from conanfile.txt, so a
// find_package(GTest QUIET) guard silently unregisters any target that names
// it, and a regression guard that does not run on CI guards nothing.
// ---------------------------------------------------------------------------
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

#define CHECK(cond, ...)                                                       \
  test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...)                                                    \
  test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

#define TEST(name)                                                             \
  do {                                                                         \
    ++g_tests;                                                                 \
    printf("  TEST: %s\n", #name);                                             \
    test_##name();                                                             \
  } while (0)

using RecipeMgr = RecipeManager::RecipeManager;
using Recipe = RecipeManager::Recipe;

namespace {

const std::string kDataDir = DATA_DIR;

// The reachability baseline, embedded as a C array rather than read from a
// data file: it is test policy, not content, and keeping it out of
// src/content/data/ means a content edit can never widen it by accident.
const char *const kUnreachableBaseline[] = {
#include "reachability_baseline.inc"
};

std::string readFile(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open())
    return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Load the real item registry exactly once; the singleton short-circuits
// subsequent calls (ItemRegistry::loadFromCSV returns early when loaded_).
void ensureRegistry() {
  static bool done = false;
  if (done)
    return;
  RecipeManager::ItemRegistry::instance().loadFromCSV(kDataDir +
                                                      "/registry/items.csv");
  done = true;
}

// The registry universe: every real item in items.csv. Read directly rather
// than derived from the recipes, because an item that appears in NO recipe is
// exactly the case this test exists to catch — enumerating the universe from
// the recipes would make that item invisible. `int,name,stack,meta` is the
// header and `#` starts a comment; both are skipped, matching
// ItemRegistry::loadFromCSV, and the count is cross-checked against it.
std::set<uint16_t> registryItems() {
  std::set<uint16_t> ids;
  std::istringstream iss(readFile(kDataDir + "/registry/items.csv"));
  std::string line;
  while (std::getline(iss, line)) {
    std::string s = line;
    while (!s.empty() && (s.back() == '\r' || s.back() == ' '))
      s.pop_back();
    const size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos || s[start] == '#')
      continue;
    const size_t comma = s.find(',');
    if (comma == std::string::npos)
      continue;
    const std::string idStr = s.substr(0, comma);
    if (idStr == "int" || idStr == "id") // header row
      continue;
    ids.insert(ItemId::pack(idStr));
  }
  return ids;
}

// Every recipe the runtime parser accepted, via the public catalog accessors.
// collectRecipeItemIds() is the deduped union of every input and output id, so
// querying each of them with mode=0 (both) walks the whole recipe table. A
// recipe naming no item at all (no item inputs, no item outputs) cannot appear
// here, but it also contributes no output to the fixpoint, so its absence
// cannot change the result.
std::vector<const Recipe *> allRecipes(RecipeMgr &mgr) {
  std::vector<const Recipe *> all;
  std::set<std::string> seen;
  for (uint16_t id : mgr.collectRecipeItemIds()) {
    for (const Recipe *r : mgr.findRecipesForItem(id, /*mode=*/0)) {
      if (seen.insert(r->id).second)
        all.push_back(r);
    }
  }
  return all;
}

// ── world base, read from GENERATOR SOURCE ─────────────────────────────────
// The ids the world generator can place with no recipe involved, transcribed
// from the source lines cited beside each. Deliberately not a hand-curated
// "starter items" list: the base grows when the generator grows.
struct WorldSeed {
  const char *id;
  const char *source;
};
const WorldSeed kGeneratorSeeds[] = {
    {"0:0:0", "WorldGenerator.cpp:22 BLOCK_AIR"},
    {"0:0:1", "WorldGenerator.cpp:23 BLOCK_STONE"},
    {"0:0:8", "WorldGenerator.cpp:24 BLOCK_GRASS"},
    {"0:0:7", "WorldGenerator.cpp:25 BLOCK_DIRT"},
    {"1111:11:0", "WorldGenerator.cpp:26 BLOCK_WATER"},
    {"0:10:11:2", "TreeGenerator.h:24 BLOCK_LOG"},
    {"0:10:11:3", "TreeGenerator.h:25 BLOCK_LEAVES"},
};

// drops.csv: a rule's RESULT is obtainable; a SOURCE that has a rule is not.
struct DropInfo {
  std::set<uint16_t> results; // obtainable
  std::set<uint16_t> sources; // substituted away, not obtainable
};
DropInfo readDrops(const std::string &csv) {
  DropInfo d;
  std::istringstream iss(csv);
  std::string line;
  while (std::getline(iss, line)) {
    std::string s = line;
    while (!s.empty() && (s.back() == '\r' || s.back() == ' '))
      s.pop_back();
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos || s[start] == '#')
      continue;
    std::vector<std::string> f;
    std::stringstream ss(s);
    std::string cell;
    while (std::getline(ss, cell, ','))
      f.push_back(cell);
    if (f.size() < 2)
      continue;
    d.sources.insert(ItemId::pack(f[0]));
    d.results.insert(ItemId::pack(f[1]));
  }
  return d;
}

// ores.json: every block id a vein can place. Parsed with a real JSON reader
// rather than by scanning for the three id-valued keys, so a vein that grows a
// new field cannot be silently missed.
std::set<uint16_t> readOreIds(const std::string &json) {
  std::set<uint16_t> ids;
  const nlohmann::json doc = nlohmann::json::parse(json);
  for (const auto &vein : doc.value("veins", nlohmann::json::array())) {
    for (const char *key : {"primary", "secondary", "sporadic"}) {
      const std::string id = vein.value(key, std::string());
      if (!id.empty())
        ids.insert(ItemId::pack(id));
    }
  }
  return ids;
}

// The items a player can actually obtain: the world base closed under
// "outputs of any recipe whose every required input is already obtainable".
std::set<uint16_t> computeCraftable(const std::vector<const Recipe *> &recipes) {
  std::set<uint16_t> craftable;
  for (const auto &seed : kGeneratorSeeds)
    craftable.insert(ItemId::pack(seed.id));
  for (uint16_t id : readOreIds(readFile(kDataDir + "/registry/ores.json")))
    craftable.insert(id);

  const DropInfo drops = readDrops(readFile(kDataDir + "/registry/drops.csv"));
  for (uint16_t id : drops.results)
    craftable.insert(id);
  // A source a drop rule substitutes away is not itself obtainable.
  for (uint16_t id : drops.sources)
    craftable.erase(id);

  // NOTE on id 0: it is both the packed id of `air` and the "no item" sentinel
  // (an empty pattern cell, an unresolvable name). It is deliberately NOT
  // removed from `craftable`: air is placed by the generator and therefore is
  // obtainable. Making it a sentinel-only value here would need a separate
  // universe-minus-{0}, and would put a permanent false positive in the
  // baseline. The sentinel cannot corrupt the fixpoint because both loops
  // below already skip item_id == 0 — an unresolved input is a requirement
  // that is never treated as satisfied, and id 0 is never added as an output.

  bool changed = true;
  while (changed) {
    changed = false;
    for (const Recipe *r : recipes) {
      if (r->outputs.empty())
        continue; // producer class (generator/boiler): no items produced
      bool satisfied = true;
      if (r->has_pattern) {
        // Positional recipe: the pattern is what matches() enforces.
        for (const auto &cell : r->pattern) {
          if (cell.item_id != 0 && craftable.find(cell.item_id) == craftable.end()) {
            satisfied = false;
            break;
          }
        }
      } else {
        for (const auto &in : r->inputs) {
          if (in.item_id != 0 && craftable.find(in.item_id) == craftable.end()) {
            satisfied = false;
            break;
          }
        }
      }
      if (!satisfied)
        continue;
      for (const auto &out : r->outputs) {
        if (out.item_id != 0 && craftable.insert(out.item_id).second)
          changed = true;
      }
    }
  }
  return craftable;
}

std::set<uint16_t> baselineIds() {
  std::set<uint16_t> ids;
  for (const char *entry : kUnreachableBaseline) {
    const std::string s(entry);
    if (s.empty() || s[0] == '#')
      continue;
    ids.insert(ItemId::pack(s));
  }
  return ids;
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The loader is the single source of truth for "is this recipe accepted", so a
// data file whose only defect is a missing or invalid `outputs` block shows up
// as a recipe the server never had, not as silently-vanishing content.
static void test_the_recipe_directory_loads_under_the_runtime_parser() {
  ensureRegistry();
  RecipeMgr mgr;
  CHECK(mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes"),
        "the real recipe directory loads");
  CHECK(mgr.recipeCount() > 0, "recipes were accepted by the runtime parser");
  printf("    %zu recipes accepted by the runtime parser\n", mgr.recipeCount());
}

// transformer_lv_mv is the bug this issue was filed for: quest 74 requires it
// as a `craft`, the four sibling transformers all had recipes, and it had none.
static void test_transformer_lv_mv_has_a_recipe() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");

  const uint16_t kItem = ItemId::pack("1110:110:2");
  CHECK(RecipeManager::ItemRegistry::instance().isValid(kItem),
        "transformer_lv_mv is a real registry item");

  const Recipe *r = mgr.getRecipeById("transformer_lv_mv");
  CHECK(r != nullptr, "transformer_lv_mv is craftable at all");
  if (r == nullptr)
    return;

  CHECK_EQ(size_t(1), r->outputs.size(), "produces exactly one output");
  if (!r->outputs.empty())
    CHECK_EQ(size_t(kItem), size_t(r->outputs[0].item_id),
             "the output is the registered transformer_lv_mv id");

  // The whole chain must be present, so a future rung cannot reintroduce a
  // missing link in the middle of the series.
  for (const char *id : {"transformer_lv_mv", "transformer_mv_hv",
                         "transformer_hv_ev", "transformer_ev_iv",
                         "transformer_iv_luv"}) {
    CHECK(mgr.getRecipeById(id) != nullptr,
          "the transformer chain is complete");
  }
}

// The precondition for the baseline comparison: the registry the model runs
// against is the real one, with the header and '#' comments excluded.
static void test_the_item_registry_is_the_one_being_audited() {
  ensureRegistry();
  const auto &reg = RecipeManager::ItemRegistry::instance();
  CHECK(reg.count() > 0, "items.csv loaded");
  CHECK(reg.isValid(ItemId::pack("0:0:1")), "a known base item resolves");
  CHECK(!reg.isValid(0xFFFF), "an unassigned id does not resolve");

  // The universe the fixpoint is subtracted from is items.csv itself, so it
  // must agree with the registry the runtime resolves recipe names against.
  const std::set<uint16_t> universe = registryItems();
  CHECK(!universe.empty(), "the item universe is non-empty");
  printf("    %zu registry items (ItemRegistry::count() = %zu)\n",
         universe.size(), reg.count());
  CHECK_EQ(reg.count(), universe.size(),
           "the audit universe is the whole registry, header excluded");

  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");
  const std::vector<const Recipe *> recipes = allRecipes(mgr);
  CHECK(!recipes.empty(), "recipes were enumerated through the public accessors");

  // The model derives its world base from the generator, so a base-block id
  // that the generator places must be reachable with no recipe at all.
  const std::set<uint16_t> craftable = computeCraftable(recipes);
  for (const auto &seed : kGeneratorSeeds) {
    const uint16_t id = ItemId::pack(seed.id);
    if (std::strcmp(seed.id, "0:0:1") == 0)
      continue; // stone: drops.csv substitutes cobblestone for it on mining
    CHECK(craftable.find(id) != craftable.end(),
          "a generator-placed block is in the world base");
  }
}

// Every quest `craft` requirement must have at least one recipe that produces
// it. This is the assertion that would have caught gp-b2pl, and it is the most
// drift-resistant invariant available: it is scoped to the items the quest
// graph actually exposes, so the unrelated dead-content backlog cannot mask a
// genuinely new breakage.
static void test_every_quest_craft_requirement_has_a_producing_recipe() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");

  const std::string json =
      readFile(kDataDir + "/quests/quest_requirements.json");
  CHECK(!json.empty(), "quest_requirements.json is readable");

  // Parsed with a real JSON reader, not a hand-rolled scan: an earlier
  // substring version of this walked into `"item"` the KEY of a requirement and
  // packed the literal string "item" (which is 0 for every quest) instead of
  // its value, so all 148 craft requirements silently collapsed onto id 0.
  // quest_requirements.json is the file the quest system actually loads, so
  // anything this test says about it has to come from a real parse.
  nlohmann::json doc = nlohmann::json::parse(json);
  std::vector<uint16_t> craftItems;
  for (const auto &quest : doc.items()) {
    if (!quest.value().is_object() || !quest.value().contains("requirements"))
      continue;
    for (const auto &req : quest.value()["requirements"]) {
      if (req.value("kind", std::string()) != "craft")
        continue;
      const std::string item = req.value("item", std::string());
      CHECK(!item.empty(), "a quest craft requirement names an item");
      craftItems.push_back(ItemId::pack(item));
    }
  }
  CHECK(!craftItems.empty(), "quest craft requirements were found");
  printf("    %zu quest craft requirements scanned\n", craftItems.size());

  // Known exception, deliberately narrow and asserted here rather than folded
  // into an unremarkable list: CreativeGeneratorSystem grants items straight
  // from its inventory path (src/game/mining/CreativeGeneratorSystem.{h,cpp})
  // and has no recipe anywhere, so quest 37's `craft creative_generator` can
  // never be satisfied by the recipe graph. That is a quest-design bug, out of
  // gp-b2pl's file scope; naming it means that when it is fixed the change is a
  // deliberate edit to this list, not silence.
  const uint16_t kCreativeGenerator = ItemId::pack("1110:100:0");

  int unsatisfied = 0;
  for (uint16_t item : craftItems) {
    if (item == kCreativeGenerator)
      continue;
    if (mgr.findRecipesForItem(item, /*mode=*/1).empty()) {
      ++unsatisfied;
      fprintf(stderr,
              "    quest craft item %u (%s) has no producing recipe\n", item,
              RecipeManager::ItemRegistry::instance().idToName(item).c_str());
    }
  }
  CHECK_EQ(0, unsatisfied,
           "every quest craft requirement has a producing recipe");
}

// TASK 2 of gp-b2pl — coolant (1111:11:5) — DECISION: coolant and its consumer
// are OUT OF SCOPE, deliberately, and this test is where that decision lives so
// it cannot be quietly re-litigated.
//
// The facts, as of this commit:
//   · coolant (1111:11:5) is in fluids.csv and items.csv, has a consumer
//     recipe (coolant_bucket, empty_bucket + coolant -> coolant_bucket) and
//     NO producer anywhere: no recipe, no ore vein, no drop rule, and no
//     machine system emits it (CoolantSystem only CONSUMES COOLANT_ITEM_ID,
//     it never credits one).
//   · quest 159 "Coolant" requires `obtain 0:11111:4` (coolant_bucket), so it
//     is unpassable for the same reason quest 74 was.
//   · A consumer without a producer is not a missing recipe; it is an
//     unfinished FEATURE. Giving coolant a source means inventing a machine and
//     a recipe for it — a content design decision, not a reachability fix, and
//     squarely outside this issue's file scope. The task said "do not invent a
//     nonsense source", and a coolant that appears from nowhere is exactly
//     that.
//
// So coolant stays stranded, and this test asserts that strand EXPLICITLY:
// if a future change gives coolant a real producer, this assertion fails and
// the person who made it has to decide what to do with quest 159 rather than
// discovering it in a bug report. Asserting the debt is what stops the
// baseline from quietly absorbing it a second time.
static void test_coolant_is_stranded_on_purpose() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");

  const uint16_t kCoolant = ItemId::pack("1111:11:5");
  const uint16_t kCoolantBucket = ItemId::pack("0:11111:4");

  CHECK(RecipeManager::ItemRegistry::instance().isValid(kCoolant),
        "coolant is a registered fluid item");
  // fluids.csv must keep agreeing with items.csv; Registry::validateReferences
  // enforces the same pairing at load time.
  const std::string fluids = readFile(kDataDir + "/registry/fluids.csv");
  CHECK(fluids.find("1111:11:5,coolant") != std::string::npos,
        "coolant is still declared in fluids.csv");

  // The consumer exists and the producer does not: that is the recorded state.
  CHECK(mgr.findRecipesForItem(kCoolant, /*mode=*/1).empty(),
        "coolant still has no producing recipe (out of scope, see above)");
  CHECK(mgr.getRecipeById("coolant_bucket") != nullptr,
        "coolant_bucket (the consumer) still exists");
  // coolant_bucket DOES have a producing recipe — its own. What strands it is
  // the coolant it consumes, so the bucket is stranded by cascade, not by a
  // missing recipe. That distinction is the whole point of the two categories
  // the reachability fixpoint distinguishes.
  CHECK(!mgr.findRecipesForItem(kCoolantBucket, /*mode=*/1).empty(),
        "coolant_bucket is craftable-in-principle from coolant");

  // coolant_bucket is stranded precisely because its input is, so it belongs in
  // the baseline; if it ever left, the ratchet would have tightened.
  const std::set<uint16_t> craftable = computeCraftable(allRecipes(mgr));
  CHECK(craftable.find(kCoolant) == craftable.end(),
        "coolant is unreachable, as recorded");
  CHECK(craftable.find(kCoolantBucket) == craftable.end(),
        "coolant_bucket is unreachable, as recorded");
  CHECK(baselineIds().count(kCoolant) == 1,
        "coolant is in the recorded baseline");
  CHECK(baselineIds().count(kCoolantBucket) == 1,
        "coolant_bucket is in the recorded baseline");
}

// The durable guard: the unreachable set must be a SUBSET of the recorded
// baseline. New strands fail by name; existing debt is tolerated; any fix
// (new ore vein, new dust recipe) only shrinks the set and passes.
static void test_unreachable_set_is_a_subset_of_the_recorded_baseline() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");

  const std::vector<const Recipe *> recipes = allRecipes(mgr);
  CHECK(!recipes.empty(), "recipes were enumerated through the public accessors");
  const std::set<uint16_t> craftable = computeCraftable(recipes);
  const std::set<uint16_t> baseline = baselineIds();
  CHECK(!baseline.empty(), "the recorded baseline is non-empty");

  int regressions = 0;
  size_t unreachable = 0;
  for (uint16_t item : registryItems()) {
    if (craftable.find(item) != craftable.end())
      continue;
    ++unreachable;
    if (baseline.find(item) == baseline.end()) {
      ++regressions;
      fprintf(stderr, "    newly unreachable item %u (%s) is not in the baseline\n",
              item,
              RecipeManager::ItemRegistry::instance().idToName(item).c_str());
    }
  }

  // Progress is the point of a subset assertion: report how much of the
  // recorded debt is now cleared, so it stays visible instead of implicit.
  size_t cleared = 0;
  for (uint16_t item : baseline) {
    if (craftable.find(item) != craftable.end())
      ++cleared;
  }
  printf("    %zu of %zu baseline entries are now reachable (%zu still dead)\n",
         cleared, baseline.size(), baseline.size() - cleared);
  printf("    %zu registry items unreachable in total\n", unreachable);
  CHECK_EQ(0, regressions,
           "no newly unreachable item outside the recorded baseline");
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("=== content reachability guard (gp-b2pl) ===\n\n");

  TEST(the_recipe_directory_loads_under_the_runtime_parser);
  TEST(the_item_registry_is_the_one_being_audited);
  TEST(transformer_lv_mv_has_a_recipe);
  TEST(every_quest_craft_requirement_has_a_producing_recipe);
  TEST(coolant_is_stranded_on_purpose);
  TEST(unreachable_set_is_a_subset_of_the_recorded_baseline);

  printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
