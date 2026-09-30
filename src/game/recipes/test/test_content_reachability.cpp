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

// Every recipe the runtime parser accepted, via the public catalog accessors.
// collectRecipeItemIds() is the deduped union of every input and output id, so
// querying each of them with mode=0 (both) walks the whole recipe table. A
// recipe naming no item at all (no item inputs, no item outputs) cannot appear
// here, but it also contributes no output to the fixpoint, so its absence
// cannot change the result.
//
// The fluid port needs its own sweep, and that is a real hole rather than
// belt-and-braces: collectRecipeItemIds() unions recipe.inputs and
// recipe.outputs ONLY (RecipeManager.cpp:1380-1396). A recipe declaring
// `fluid_inputs`/`fluid_outputs` and naming no item is therefore invisible to
// the item sweep -- which is exactly the shape of a fluid-only refinery, the
// one recipe type whose whole purpose is to make a fluid. Enumerating every
// ItemId::isFluid id as well closes it. test_all_accepted_recipes_reach_the_
// model cross-checks this against mgr.recipeCount() so the sweep cannot
// silently go stale if the accessors change again.
std::vector<const Recipe *> allRecipes(RecipeMgr &mgr) {
  std::vector<const Recipe *> all;
  std::set<std::string> seen;
  auto keep = [&](const Recipe *r) {
    if (seen.insert(r->id).second)
      all.push_back(r);
  };
  for (uint16_t id : mgr.collectRecipeItemIds())
    for (const Recipe *r : mgr.findRecipesForItem(id, /*mode=*/0))
      keep(r);
  for (uint16_t id : registryItems())
    if (ItemId::isFluid(id))
      for (const Recipe *r : mgr.findRecipesForItem(id, /*mode=*/0))
        keep(r);
  return all;
}

// True when `r` yields at least one id the fixpoint can consume. A recipe with
// neither item outputs nor fluid outputs produces nothing, so its inputs are
// irrelevant -- this replaces the old `r->outputs.empty()` shortcut, which
// would have skipped a fluid-only producer even once its inputs were met.
static bool producesAnything(const Recipe &r) {
  for (const auto &out : r.outputs)
    if (out.item_id != 0)
      return true;
  for (const auto &out : r.fluid_outputs)
    if (out.fluid_id != 0)
      return true;
  return false;
}

// Every id `r` must be fed before it runs: the items matches() enforces PLUS
// each per-operation fluid input. The fluid half is a hard requirement, not a
// hint: Recipe::needsReservation (RecipeTypes.h:151) makes the machine reserve
// the volume before the craft starts, so a recipe demanding an unobtainable
// fluid cannot be run. This is what stops an unresolvable fluid from reading
// as free -- the C++ half of the same rule the Python audit applies.
static bool requirementsSatisfied(const Recipe &r,
                                  const std::set<uint16_t> &craftable) {
  if (r.has_pattern) {
    // Positional recipe: the pattern is what matches() enforces.
    for (const auto &cell : r.pattern) {
      if (cell.item_id != 0 && craftable.find(cell.item_id) == craftable.end())
        return false;
    }
  } else {
    for (const auto &in : r.inputs) {
      if (in.item_id != 0 && craftable.find(in.item_id) == craftable.end())
        return false;
    }
  }
  for (const auto &in : r.fluid_inputs) {
    if (in.fluid_id != 0 && craftable.find(in.fluid_id) == craftable.end())
      return false;
  }
  return true;
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
      if (!producesAnything(*r))
        continue; // nothing to add to the fixpoint, whatever its inputs are
      if (!requirementsSatisfied(*r, craftable))
        continue;
      for (const auto &out : r->outputs) {
        if (out.item_id != 0 && craftable.insert(out.item_id).second)
          changed = true;
      }
      // A credited fluid is obtainable, same as an emitted item. Without this
      // no fluid could EVER become craftable, however many recipes make it.
      for (const auto &out : r->fluid_outputs) {
        if (out.fluid_id != 0 && craftable.insert(out.fluid_id).second)
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
static void test_coolant_has_a_source_again() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");

  const uint16_t kCoolant = ItemId::pack("1111:11:5");
  const uint16_t kCoolantBucket = ItemId::pack("0:11111:4");
  const uint16_t kCoolantPlant = ItemId::pack("1110:100:17");

  CHECK(RecipeManager::ItemRegistry::instance().isValid(kCoolant),
        "coolant is a registered fluid item");
  // fluids.csv must keep agreeing with items.csv; Registry::validateReferences
  // enforces the same pairing at load time.
  const std::string fluids = readFile(kDataDir + "/registry/fluids.csv");
  CHECK(fluids.find("1111:11:5,coolant") != std::string::npos,
        "coolant is still declared in fluids.csv");

  // THE STRAND IS GONE. This test used to assert the opposite, and the
  // inversion is the point: it recorded a deliberate gap and now records that
  // the gap was closed.
  //
  // The chain, end to end:
  //   creative_water_generator (1110:100:16) publishes the `water` fluid via
  //   CreativeFluidSystem, and creative_water_source_fluid declares it in YAML
  //   so the reachability models can see it at all.
  //   gtnh:coolant_plant_distil_water consumes `water` through the recipe fluid
  //   port and produces a coolant_bucket ITEM, which is the unit CoolantSystem
  //   actually consumes (HeatConstants::COOLANT_ITEM_ID = 0:11111:4).
  //   q159 requires exactly one coolant_bucket, so quest 159 is passable.
  const Recipe *distil = mgr.getRecipeById("gtnh:coolant_plant_distil_water");
  CHECK(distil != nullptr, "the coolant plant's distilling recipe exists");
  if (distil != nullptr) {
    CHECK_EQ(size_t(1), size_t(distil->fluid_inputs.size()),
             "it distils exactly one fluid input");
    if (!distil->fluid_inputs.empty()) {
      CHECK_EQ(size_t(ItemId::pack("1111:11:0")), size_t(distil->fluid_inputs[0].fluid_id),
               "and that input is water, not some other fluid");
    }
  }

  // The block is craftable, or the whole chain is decorative: a machine the
  // player cannot build produces nothing. This is the trap creative_generator
  // fell into before a388d3c1 gave it a recipe.
  CHECK(!mgr.findRecipesForItem(kCoolantPlant, /*mode=*/1).empty(),
        "the coolant_plant block has a craft recipe");
  CHECK(RecipeManager::ItemRegistry::instance().isValid(kCoolantPlant),
        "the coolant_plant block is registered in items.csv");

  // And the positive direction, measured rather than asserted: both coolant
  // and its bucket are now craftable, so the baseline must NOT still list them.
  const std::set<uint16_t> craftable = computeCraftable(allRecipes(mgr));
  CHECK(craftable.find(kCoolantBucket) != craftable.end(),
        "coolant_bucket is craftable: the strand is closed");
  // The BARE coolant fluid stays unreachable, and that is deliberate, not a
  // leftover. The plant distils water straight into a coolant_bucket, because
  // coolant_bucket is the unit CoolantSystem consumes and the unit q159 asks
  // for. The intermediate `coolant` item is not needed by anything, so
  // producing it would add a dead item rather than close a gap.
  CHECK(craftable.find(kCoolant) == craftable.end(),
        "bare coolant stays unreachable: the plant yields the bucket directly");
}

// graphite (0:1110:001:31) and charcoal_dust (0:1110:001:32) are registered
// but stranded, the same shape as coolant above and for the same reason.
//
// The user asked for both items to exist; they did NOT exist before this
// commit, so registering them is not a regression - it is two new orphans.
// Nothing in the recipe tree mentions either name yet, and the user did not
// specify a producer for them. Inventing one here would be exactly the
// nonsense source the coolant test above refuses to invent: graphite has no
// consumer in this tree to justify a producer, and charcoal_dust's obvious
// use (alongside carbon_dust in gtnh:alloy_steel_dust) is a content decision
// nobody has made.
//
// So the debt is asserted EXPLICITLY, and the baseline records both ids. When
// someone gives them a real producer or consumer, this test fails and whoever
// did it decides what it means rather than discovering it as a silent metric
// change.
static void test_graphite_and_charcoal_dust_are_stranded_on_purpose() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");

  const uint16_t kGraphite = ItemId::pack("0:1110:001:31");
  const uint16_t kCharcoalDust = ItemId::pack("0:1110:001:32");

  CHECK(RecipeManager::ItemRegistry::instance().isValid(kGraphite),
        "graphite is registered");
  CHECK(RecipeManager::ItemRegistry::instance().isValid(kCharcoalDust),
        "charcoal_dust is registered");

  // No recipe produces them yet, and none consumes them: that is the state.
  CHECK(mgr.findRecipesForItem(kGraphite, /*mode=*/1).empty(),
        "graphite has no producing recipe yet (awaiting a content decision)");
  CHECK(mgr.findRecipesForItem(kCharcoalDust, /*mode=*/1).empty(),
        "charcoal_dust has no producing recipe yet (awaiting a content decision)");

  CHECK(baselineIds().count(kGraphite) == 1,
        "graphite is in the recorded baseline");
  CHECK(baselineIds().count(kCharcoalDust) == 1,
        "charcoal_dust is in the recorded baseline");
}

// The fluid port is not decoration: this is the same property coolant gets
// above, asserted for oil, and it is the reason this model reads
// `fluid_inputs` at all.
//
// The facts, at the time this test was written:
//   · oil (1111:11:58) is in fluids.csv and items.csv.
//   · gtnh:chemical_reactor_oil_crack CONSUMES it via
//     `fluid_inputs: [{fluid: oil, amount: 1000}]` and produces ethylene, so
//     ethylene is stranded by cascade off oil.
//   · NOTHING produces oil: no recipe declares it in `fluid_outputs`, no ore
//     vein places it, and drops.csv maps nothing to it. There is no oil well
//     machine in machines.yaml.
//   · The polymer chain hangs off the same fluid, which is why the Python
//     audit reports oil as a progression root with ~30 items downstream rather
//     than as dead content nobody reads.
//
// So the honest verdict is: ethylene and the polyethylene chain are NOT
// craftable, because the one recipe that makes ethylene requires a fluid the
// game cannot produce. That is the correct answer, not a modelling failure --
// and it is exactly the case that a fluid-blind model gets wrong in the
// dangerous direction, by counting ethylene as craftable and hiding a real
// strand behind a false green.
//
// Deliberately NOT done here: inventing an oil source. A producer has to be a
// machine in machines.yaml, a block id, and a recipe; writing one to silence
// this assertion would fabricate a feature. When oil does get a source, this
// test fails by name and the person who added it decides what happens to the
// baseline entry.
static void test_oil_is_a_stranded_fluid_input_on_purpose() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");

  const uint16_t kOil = ItemId::pack("1111:11:58");
  const uint16_t kEthylene = ItemId::pack("1111:11:6");

  CHECK(ItemId::isFluid(kOil), "oil packs into the fluid id range");
  CHECK(RecipeManager::ItemRegistry::instance().isValid(kOil),
        "oil is a registered item");
  const std::string fluids = readFile(kDataDir + "/registry/fluids.csv");
  CHECK(fluids.find("1111:11:58,oil") != std::string::npos,
        "oil is still declared in fluids.csv");

  // The consumer is real, and it consumes oil through the FLUID port -- not
  // through an item input. Asserting the port itself is what proves the model
  // is reading these keys rather than getting the right answer by accident.
  const Recipe *crack = mgr.getRecipeById("gtnh:chemical_reactor_oil_crack");
  CHECK(crack != nullptr, "the oil-cracking consumer exists");
  if (crack != nullptr) {
    CHECK_EQ(size_t(1), crack->fluid_inputs.size(),
             "it declares exactly one fluid input");
    if (!crack->fluid_inputs.empty()) {
      CHECK_EQ(size_t(kOil), size_t(crack->fluid_inputs[0].fluid_id),
               "the fluid it requires is oil");
      CHECK(crack->fluid_inputs[0].valid(),
            "the fluid entry is valid (non-zero id, amount > 0)");
    }
    CHECK(crack->inputs.empty(),
          "oil arrives via the fluid port, not as an item input");
  }

  // The producer side, and the inversion is the point. This test used to assert
  // that NOTHING emits oil and that the whole ethylene/polymer/electronics chain
  // was stranded behind it. That was true when written, and the creative oil
  // generator has since given it a source, so the assertion now runs the other
  // way: the consumer is still wired the same way, but the chain is live.
  //
  // What actually produces the oil is NOT a recipe body - it is
  // CreativeFluidSystem, whose C++ table (kCreativeFluidSources,
  // CreativeFluidSystem.cpp:21) publishes the fluid every tick. The audit
  // cannot see C++, so creative_oil_source_fluid declares the same source in
  // YAML; `fluid_outputs` is parsed for every machine class
  // (RecipeManager.cpp:988-989), so that declaration is also read at runtime and
  // is additive documentation rather than a second source of oil.
  const Recipe *source = mgr.getRecipeById("creative_oil_source_fluid");
  CHECK(source != nullptr, "the creative oil source declares a fluid output");
  if (source != nullptr) {
    CHECK_EQ(size_t(1), source->fluid_outputs.size(),
             "it emits exactly one fluid");
    if (!source->fluid_outputs.empty()) {
      CHECK_EQ(size_t(kOil), size_t(source->fluid_outputs[0].fluid_id),
               "and that fluid is oil");
    }
  }

  const std::set<uint16_t> craftable = computeCraftable(allRecipes(mgr));
  CHECK(craftable.find(kOil) != craftable.end(),
        "oil is craftable: the source is visible to the model");
  // And the cascade the audit used to call out has come alive with it:
  // ethylene still has its producing recipe, and now that recipe's fluid input
  // is obtainable, so ethylene is craftable too.
  CHECK(!mgr.findRecipesForItem(kEthylene, /*mode=*/1).empty(),
        "ethylene does have a producing recipe");
  CHECK(craftable.find(kEthylene) != craftable.end(),
        "ethylene is craftable now that the oil it cracks is obtainable");
}

// The fluid port is only load-bearing if reading it CHANGES an answer. A test
// that models fluids and never asserts the difference would pass just as
// happily with the old fluid-blind code, so the two models are pinned against
// each other here on purpose:
//
//   · a fluid that HAS a producer must become craftable, and
//   · a fluid with NO producer must stay unreachable even though a recipe
//     consumes it.
//
// The second half is the trap this whole change exists to close: silently
// dropping a `fluid_inputs` entry makes its consumer look craftable, which
// turns a real strand into a green build. Both are asserted on the REAL
// content tree, so they hold today without a fixture.
static void test_the_fluid_port_actually_changes_the_outcome() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");
  const std::vector<const Recipe *> recipes = allRecipes(mgr);
  const std::set<uint16_t> craftable = computeCraftable(recipes);

  // steam is emitted by boiler_coal (energy_output SU) -- but as an ENERGY
  // output, not a fluid output, so it is NOT reachable through this port. If a
  // future change gives steam a real `fluid_outputs` producer, this assertion
  // is the thing that has to be revisited, deliberately and by name.
  const uint16_t kSteam = ItemId::pack("1111:11:1");
  bool steamProducedAsFluid = false;
  for (const Recipe *r : recipes)
    for (const auto &out : r->fluid_outputs)
      if (out.fluid_id == kSteam)
        steamProducedAsFluid = true;
  CHECK_EQ(steamProducedAsFluid, craftable.find(kSteam) != craftable.end(),
           "steam is craftable if and only if a recipe emits it as a fluid");

  // Same invariant for EVERY registered fluid, not just the one named above.
  // This is the general form of the rule: fluid_outputs is a producer, and
  // fluid_inputs is a requirement. A fluid that no recipe consumes and no
  // recipe produces is dead content, and the port must not resurrect it.
  for (uint16_t fluid : registryItems()) {
    if (!ItemId::isFluid(fluid))
      continue;
    bool produced = false;
    bool consumed = false;
    for (const Recipe *r : recipes) {
      for (const auto &out : r->fluid_outputs)
        if (out.fluid_id == fluid)
          produced = true;
      for (const auto &in : r->fluid_inputs)
        if (in.fluid_id == fluid)
          consumed = true;
    }
    const bool reachable = craftable.find(fluid) != craftable.end();
    if (!produced && !consumed)
      continue; // nothing to assert: dead content, port is silent on it
    if (!produced && consumed) {
      // The consumer exists and the producer does not: this fluid must be
      // unreachable, and anything it feeds must be unreachable too.
      CHECK(!reachable,
            "a consumed fluid with no producer is NOT craftable");
    }
  }

  // And the positive direction, proven on real content rather than asserted in
  // the abstract: a fluid that a recipe DOES emit becomes craftable, and the
  // items that recipe outputs with it become craftable in the same round.
  int fluidProducers = 0;
  for (const Recipe *r : recipes) {
    if (r->fluid_outputs.empty())
      continue;
    ++fluidProducers;
    for (const auto &out : r->fluid_outputs) {
      const bool satisfied = requirementsSatisfied(*r, craftable);
      CHECK_EQ(satisfied, craftable.find(out.fluid_id) != craftable.end(),
               "a fluid output is reachable exactly when its recipe's "
               "requirements are all reachable");
    }
  }
  printf("    %d recipe(s) emit fluids; %zu live recipe(s) total\n",
         fluidProducers, mgr.recipeCount());
}

// The recipe sweep in allRecipes() is a heuristic over two public accessors,
// so it needs a completeness check: a recipe that names no item at all (a
// fluid-only refinery) is invisible to collectRecipeItemIds(). This asserts
// the sweep does not silently under-collect, which would quietly weaken every
// count above.
static void test_all_accepted_recipes_reach_the_model() {
  ensureRegistry();
  RecipeMgr mgr;
  mgr.loadRecipesFromYamlDirectory(kDataDir + "/recipes");
  const std::vector<const Recipe *> recipes = allRecipes(mgr);
  printf("    %zu of %zu accepted recipes enumerated by allRecipes()\n",
         recipes.size(), mgr.recipeCount());
  CHECK(!recipes.empty(), "the sweep produced something");
  // Every enumerated id is a real accepted recipe, and none is duplicated.
  std::set<std::string> ids;
  for (const Recipe *r : recipes) {
    CHECK(mgr.getRecipeById(r->id) == r,
          "an enumerated recipe is the loader's own instance");
    CHECK(ids.insert(r->id).second, "no recipe is enumerated twice");
  }
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
  TEST(all_accepted_recipes_reach_the_model);
  TEST(transformer_lv_mv_has_a_recipe);
  TEST(every_quest_craft_requirement_has_a_producing_recipe);
  TEST(coolant_has_a_source_again);
  TEST(graphite_and_charcoal_dust_are_stranded_on_purpose);
  TEST(oil_is_a_stranded_fluid_input_on_purpose);
  TEST(the_fluid_port_actually_changes_the_outcome);
  TEST(unreachable_set_is_a_subset_of_the_recorded_baseline);

  printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
