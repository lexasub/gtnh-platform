#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <filesystem>
#include <unistd.h>

#include <flatbuffers/flatbuffers.h>
#include <engine/registry/ItemId.h>
#include <game/recipes/ItemRegistry.h>
#include <game/recipes/RecipeManager.h>

extern int g_tests, g_passed, g_failed;
void test_check(bool cond, const char* file, int line, const char* expr, const char* msg);

#ifndef CHECK
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#endif
#ifndef CHECK_EQ
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#endif
#ifndef CHECK_NE
#define CHECK_NE(a, b, msg) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, msg)
#endif
#ifndef CHECK_GT
#define CHECK_GT(a, b, msg) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, msg)
#endif
#ifndef PASS
#define PASS() do { ++g_passed; } while(0)
#endif
#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

static std::string makeTempFile(const std::string& content) {
    char tmpl[] = "/tmp/recipe_mgr_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return {};
    [[maybe_unused]] ssize_t wr = write(fd, content.data(), content.size());
    close(fd);
    return std::string(tmpl);
}

static void test_recipe_manager_empty() {
    RecipeManager::RecipeManager mgr;
    CHECK_EQ(mgr.recipeCount(), size_t(0), "no recipes initially");

    auto* r = mgr.getRecipeById("nonexistent");
    CHECK(r == nullptr, "unknown recipe returns nullptr");

    auto* found = mgr.findRecipeByInputs(0, {});
    CHECK(found == nullptr, "no inputs returns nullptr");

    PASS();
}

static void test_recipe_manager_load_crafting_table() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    RecipeManager::RecipeManager mgr;
    bool ok = mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes");
    CHECK(ok, "loaded YAML recipes from data/recipes/");
    CHECK_GT(mgr.recipeCount(), size_t(0), "at least one recipe loaded");

    auto* stick = mgr.getRecipeById("stick");
    if (stick) {
        CHECK_NE(stick->id, std::string(""), "stick recipe has id");
        CHECK_GT(stick->duration, uint32_t(0), "stick recipe has duration > 0");
        CHECK(!stick->inputs.empty(), "stick recipe has inputs");
        CHECK(!stick->outputs.empty(), "stick recipe has outputs");
    }

    PASS();
}

static void test_recipe_manager_find_stick() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    RecipeManager::RecipeManager mgr;
    mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes");
    mgr.loadMachinesFromYaml(DATA_DIR "/registry/machines.yaml");

    // Verify recipes exist by id.
    auto* ct = mgr.getRecipeById("crafting_table");
    CHECK_NE(ct, nullptr, "crafting_table recipe exists");
    auto* stick = mgr.getRecipeById("stick");
    CHECK_NE(stick, nullptr, "stick recipe exists");
    if (stick) {
        CHECK_GT(stick->duration, uint32_t(0), "stick recipe has duration > 0");
        CHECK(!stick->inputs.empty(), "stick recipe has inputs");
        CHECK(!stick->outputs.empty(), "stick recipe has outputs");
    }

    // Crafting-table matching is positional (3x3 pattern): a single plank does
    // not match anything, but two planks stacked vertically = stick.
    const uint16_t kPlank = ItemId::pack("0:10:00:0");
    std::vector<RecipeManager::ItemStack> lonePlank = {{kPlank, 1, 0}};
    CHECK(mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), lonePlank) == nullptr,
          "single plank matches nothing (positional 3x3)");

    std::vector<RecipeManager::ItemStack> stickPattern = {
        {kPlank, 1, 0}, {0, 0, 0}, {0, 0, 0},
        {kPlank, 1, 0}, {0, 0, 0}, {0, 0, 0},
        {0, 0, 0},      {0, 0, 0}, {0, 0, 0},
    };
    auto* recipe = mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), stickPattern);
    CHECK_NE(recipe, nullptr, "stick pattern (2 planks vertical) matches");
    CHECK_EQ(recipe->id, std::string("stick"), "matches the stick recipe");
    if (recipe && !recipe->outputs.empty())
        CHECK_EQ(recipe->outputs[0].item_id, uint16_t(ItemId::pack("0:11110:0")),
                 "stick recipe outputs stick");

    PASS();
}

// All three item formats (hierarchical, flat numeric, string name) must resolve
// to the same packed uint16_t through the YAML parser, and hierarchical ids must
// not silently parse as 0 (the old std::stoi("0:0:4") -> 0 bug).
static void test_recipe_manager_item_id_formats() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");

    const char* kYaml = R"(
class: furnace
recipes:
  - name: fmt_hierarchical
    inputs:
      - { item: 0:0:4, count: 1 }
    outputs:
      - { item: 4, count: 1 }
    duration: 100
  - name: fmt_flat
    inputs:
      - { item: 4, count: 1 }
    outputs:
      - { item: 4, count: 1 }
    duration: 100
  - name: fmt_name
    inputs:
      - { item: glass, count: 1 }
    outputs:
      - { item: 4, count: 1 }
    duration: 100
)";

    auto tmp = std::filesystem::temp_directory_path() /
               ("gtnh_recipe_id_format_" + std::to_string(::getpid()) + ".yaml");
    {
        std::ofstream out(tmp);
        out << kYaml;
    }

    RecipeManager::RecipeManager mgr;
    bool ok = mgr.loadRecipesFromYamlFile(tmp.string());
    std::filesystem::remove(tmp);

    CHECK(ok, "temp YAML with all three item formats loads");
    if (!ok) { PASS(); return; }

    const uint16_t kExpected = ItemId::pack("0:0:4"); // glass
    CHECK_EQ(kExpected, uint16_t(4), "pack('0:0:4') == 4");

    for (const char* name : {"fmt_hierarchical", "fmt_flat", "fmt_name"}) {
        auto* r = mgr.getRecipeById(name);
        CHECK_NE(r, nullptr, name);
        if (r) {
            CHECK_EQ(r->inputs.size(), size_t(1), name);
            if (r->inputs.size() == 1) {
                CHECK_EQ(r->inputs[0].item_id, kExpected, name);
                CHECK_NE(r->inputs[0].item_id, uint16_t(0), name);
            }
        }
    }

    PASS();
}

static void dumpMatch(RecipeManager::RecipeManager& mgr,
                      const char* label,
                      const std::vector<RecipeManager::ItemStack>& grid) {
    auto* recipe = mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), grid);
    if (!recipe) {
        printf("  [%s] NO MATCH\n", label);
        return;
    }
    printf("  [%s] MATCHED recipe='%s' outputs=%zu\n",
           label, recipe->id.c_str(), recipe->outputs.size());
    if (!recipe->outputs.empty()) {
        printf("  [%s] output item=%u count=%u\n",
               label, recipe->outputs[0].item_id, recipe->outputs[0].count);
    }
    auto crafted = recipe->craft(grid);
    printf("  [%s] grid after craft: ", label);
    for (auto& s : crafted) printf("%u/", s.item_id);
    printf("\n");
}

static void test_recipe_manager_craft_patterns() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    RecipeManager::RecipeManager mgr;
    mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes");
    mgr.loadMachinesFromYaml(DATA_DIR "/registry/machines.yaml");

    const uint16_t kPlank = ItemId::pack("0:10:00:0");
    const uint16_t kStick = ItemId::pack("0:11110:0");
    const uint16_t kCobble = ItemId::pack("0:0:2");
    const uint16_t kIron = ItemId::pack("0:110:1");

    // wooden_pickaxe pattern (client kRecipes): 3 planks top, 2 sticks col 1
    std::vector<RecipeManager::ItemStack> pickaxe = {
        {kPlank,1,0},{kPlank,1,0},{kPlank,1,0},
        {0,0,0},     {kStick,1,0},{0,0,0},
        {0,0,0},     {kStick,1,0},{0,0,0},
    };
    dumpMatch(mgr, "pickaxe", pickaxe);
    auto* pickRecipe = mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), pickaxe);
    CHECK_NE(pickRecipe, nullptr, "pickaxe pattern matches");
    if (pickRecipe) CHECK_EQ(pickRecipe->id, std::string("wooden_pickaxe"),
                             "pickaxe pattern matches wooden_pickaxe, not crafting_table");
    if (pickRecipe) {
        auto consumed = pickRecipe->consumeInputs(pickaxe);
        bool allZero = true;
        for (const auto& s : consumed)
            if (s.item_id != 0) { allZero = false; break; }
        CHECK(allZero, "consumeInputs clears all 5 pattern cells (no output in grid)");
    }

    // crafting_table 2x2: 4 planks
    std::vector<RecipeManager::ItemStack> ct = {
        {kPlank,1,0},{kPlank,1,0},{0,0,0},
        {kPlank,1,0},{kPlank,1,0},{0,0,0},
        {0,0,0},     {0,0,0},     {0,0,0},
    };
    dumpMatch(mgr, "crafting_table", ct);
    auto* ctRecipe = mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), ct);
    CHECK_NE(ctRecipe, nullptr, "crafting_table 2x2 matches");
    if (ctRecipe) CHECK_EQ(ctRecipe->id, std::string("crafting_table"),
                           "2x2 planks matches crafting_table recipe");

    // stick: 2 planks vertical
    std::vector<RecipeManager::ItemStack> stickPat = {
        {kPlank,1,0},{0,0,0},{0,0,0},
        {kPlank,1,0},{0,0,0},{0,0,0},
        {0,0,0},     {0,0,0},{0,0,0},
    };
    dumpMatch(mgr, "stick", stickPat);
    auto* stickRecipe = mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), stickPat);
    CHECK_NE(stickRecipe, nullptr, "stick pattern matches");
    if (stickRecipe) CHECK_EQ(stickRecipe->id, std::string("stick"),
                              "vertical planks matches stick, not crafting_table");

    // furnace: 8 cobblestone ring
    std::vector<RecipeManager::ItemStack> furnace = {
        {kCobble,1,0},{kCobble,1,0},{kCobble,1,0},
        {kCobble,1,0},{0,0,0},     {kCobble,1,0},
        {kCobble,1,0},{kCobble,1,0},{kCobble,1,0},
    };
    dumpMatch(mgr, "furnace", furnace);
    auto* furnRecipe = mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), furnace);
    CHECK_NE(furnRecipe, nullptr, "furnace ring matches");
    if (furnRecipe) CHECK_EQ(furnRecipe->id, std::string("furnace"),
                             "cobble ring matches furnace, not crafting_table");

    // iron_pickaxe: 3 iron top, 2 sticks col 1
    std::vector<RecipeManager::ItemStack> ironPick = {
        {kIron,1,0},{kIron,1,0},{kIron,1,0},
        {0,0,0},    {kStick,1,0},{0,0,0},
        {0,0,0},    {kStick,1,0},{0,0,0},
    };
    dumpMatch(mgr, "iron_pickaxe", ironPick);
    auto* ironRecipe = mgr.findRecipeByInputs(ItemId::pack("0:10:11:1"), ironPick);
    CHECK_NE(ironRecipe, nullptr, "iron pickaxe pattern matches");
    if (ironRecipe) CHECK_EQ(ironRecipe->id, std::string("iron_pickaxe"),
                             "iron pattern matches iron_pickaxe");

    PASS();
}

static void test_recipe_manager_no_match() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    RecipeManager::RecipeManager mgr;
    mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes");

    std::vector<RecipeManager::ItemStack> nonsense = {
        {99, 1, 0},
        {98, 1, 0},
    };
    auto* recipe = mgr.findRecipeByInputs(0, nonsense);
    CHECK(recipe == nullptr, "bogus items match nothing");

    PASS();
}

// unlock_era gating: the YAML parser reads the optional field (default 0), and
// the RecipeInfo serializer round-trips it so the client can apply its UX filter.
static void test_recipe_manager_unlock_era() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");

    const char* kYaml = R"(
class: ebf
recipes:
  - name: gated_recipe
    inputs:
      - { item: 3, count: 1 }
    outputs:
      - { item: 4, count: 2 }
    duration: 400
    min_tier: 0
    max_tier: 32767
    unlock_era: 2
  - name: always_visible
    inputs:
      - { item: 5, count: 1 }
    outputs:
      - { item: 6, count: 2 }
    duration: 400
    min_tier: 0
    max_tier: 32767
)";

    auto tmp = std::filesystem::temp_directory_path() /
               ("gtnh_recipe_unlock_era_" + std::to_string(::getpid()) + ".yaml");
    {
        std::ofstream out(tmp);
        out << kYaml;
    }

    RecipeManager::RecipeManager mgr;
    bool ok = mgr.loadRecipesFromYamlFile(tmp.string());
    std::filesystem::remove(tmp);
    CHECK(ok, "temp YAML with unlock_era loads");
    if (!ok) { PASS(); return; }

    auto* gated = mgr.getRecipeById("gated_recipe");
    CHECK_NE(gated, nullptr, "gated_recipe exists");
    if (gated) {
        CHECK_EQ(gated->unlock_era, uint8_t(2), "gated recipe parsed unlock_era = 2");
    }

    auto* open = mgr.getRecipeById("always_visible");
    CHECK_NE(open, nullptr, "always_visible exists");
    if (open) {
        CHECK_EQ(open->unlock_era, uint8_t(0), "recipe without unlock_era defaults to 0");
    }

    // buildRecipeInfo must carry unlock_era to the client.
    flatbuffers::FlatBufferBuilder builder;
    auto off = RecipeManager::RecipeManager::buildRecipeInfo(builder, *gated);
    builder.Finish(off);
    auto* info = flatbuffers::GetRoot<Protocol::RecipeInfo>(builder.GetBufferPointer());
    CHECK_NE(info, nullptr, "buildRecipeInfo produces a valid RecipeInfo");
    if (info) {
        CHECK_EQ(info->unlock_era(), uint8_t(2), "RecipeInfo serializes unlock_era");
    }

    PASS();
}

// Client-driven queries: catalog, per-item (craft/use), per-machine, and the
// RecipeInfo serializer the wire handlers build on.
static void test_recipe_manager_queries() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    RecipeManager::RecipeManager mgr;
    mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes");
    mgr.loadMachinesFromYaml(DATA_DIR "/registry/machines.yaml");

    const uint16_t kPlank = ItemId::pack("0:10:00:0");
    const uint16_t kStick = ItemId::pack("0:11110:0");
    const uint16_t kCraftingTable = ItemId::pack("0:10:11:1");

    // Catalog = union of all input/output item ids.
    auto ids = mgr.collectRecipeItemIds();
    CHECK(!ids.empty(), "catalog is non-empty");
    bool hasPlank = false, hasStick = false;
    for (auto id : ids) {
        if (id == kPlank) hasPlank = true;
        if (id == kStick) hasStick = true;
    }
    CHECK(hasPlank, "catalog includes oak_planks (a crafting input)");
    CHECK(hasStick, "catalog includes stick (a crafting output)");

    // Per-item, craft direction: stick is produced by the 'stick' recipe.
    auto stickCraft = mgr.findRecipesForItem(kStick, 1);
    bool hasStickRecipe = false;
    for (auto* r : stickCraft) if (r->id == "stick") hasStickRecipe = true;
    CHECK(hasStickRecipe, "findRecipesForItem(stick, craft) finds the stick recipe");

    // Per-item, use direction: stick is consumed by wooden_pickaxe.
    auto stickUse = mgr.findRecipesForItem(kStick, 2);
    bool hasPickaxeUse = false;
    for (auto* r : stickUse) if (r->id == "wooden_pickaxe") hasPickaxeUse = true;
    CHECK(hasPickaxeUse, "findRecipesForItem(stick, use) finds wooden_pickaxe");

    // Per-machine: crafting table class lists the crafting_table recipe.
    auto ctRecipes = mgr.findRecipesForMachine(kCraftingTable);
    CHECK(!ctRecipes.empty(), "crafting_table machine has recipes");
    bool hasCt = false;
    for (auto* r : ctRecipes) if (r->id == "crafting_table") hasCt = true;
    CHECK(hasCt, "crafting_table machine list includes the crafting_table recipe");
    CHECK(mgr.findRecipesForMachine(12345).empty(), "unknown machine returns empty");

    // RecipeInfo serializer round-trips the stick recipe.
    auto* stick = mgr.getRecipeById("stick");
    CHECK_NE(stick, nullptr, "stick recipe exists");
    if (stick) {
        flatbuffers::FlatBufferBuilder builder;
        auto off = RecipeManager::RecipeManager::buildRecipeInfo(builder, *stick);
        builder.Finish(off);
        auto* info = flatbuffers::GetRoot<Protocol::RecipeInfo>(builder.GetBufferPointer());
        CHECK_NE(info, nullptr, "buildRecipeInfo produces a valid RecipeInfo");
        if (info) {
            CHECK_NE(info->recipe_id(), nullptr, "recipe_id present");
            if (info->recipe_id())
                CHECK_EQ(std::string(info->recipe_id()->c_str()), std::string("stick"),
                         "recipe_id round-trips");
            CHECK(info->has_pattern(), "stick recipe is positional");
            auto* pat = info->pattern();
            CHECK_NE(pat, nullptr, "pattern vector present");
            if (pat) CHECK_EQ(pat->size(), size_t(9), "pattern has 9 cells");
            auto* outs = info->outputs();
            CHECK_NE(outs, nullptr, "outputs present");
        }
    }

    // Wire handlers produce valid RecipeFrame replies (req_id echoed).
    {
        auto bytes = mgr.handleCatalogRequest(42);
        flatbuffers::Verifier v(bytes.data(), bytes.size());
        CHECK(Protocol::VerifyRecipeFrameBuffer(v), "catalog reply is a valid RecipeFrame");
        auto* frame = flatbuffers::GetRoot<Protocol::RecipeFrame>(bytes.data());
        CHECK_NE(frame, nullptr, "catalog reply parses");
        if (frame) {
            CHECK_EQ(frame->payload_type(), Protocol::RecipePayload_RecipeReply,
                     "catalog reply payload is RecipeReply");
            if (frame->payload_type() == Protocol::RecipePayload_RecipeReply) {
                auto* reply = frame->payload_as_RecipeReply();
                CHECK_NE(reply, nullptr, "catalog reply has RecipeReply");
                if (reply) {
                    CHECK_EQ(reply->req_id(), uint32_t(42), "catalog reply echoes req_id");
                    CHECK_NE(reply->response_as_RecipeCatalogResp(), nullptr,
                             "catalog reply carries RecipeCatalogResp");
                }
            }
        }
    }

    PASS();
}

// The extractor recipe remains EU-powered for the LV electric variant;
// steam extractor recipes use the separate steam variant in the same class.
static void test_recipe_manager_extractor_variants() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    RecipeManager::RecipeManager mgr;
    CHECK(mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes"),
          "extractor recipes load");
    CHECK(mgr.loadMachinesFromYaml(DATA_DIR "/registry/machines.yaml"),
          "machine variants load");

    const auto* recipe = mgr.getRecipeById("gtnh:extract_rubber_wood");
    CHECK_NE(recipe, nullptr, "rubber wood extractor recipe exists");
    if (recipe) {
        CHECK_EQ(recipe->energy_type,
                 static_cast<uint8_t>(0),
                 "rubber wood uses electricity");
        const auto validation_errors = mgr.validateResourceRequirements(nullptr);
        CHECK(validation_errors.empty(),
              "extractor EU requirement matches an electric extractor variant");
        CHECK_EQ(recipe->resource_requirements.size(), size_t(1),
                 "rubber wood has one resource requirement");
        if (recipe->resource_requirements.size() == 1) {
            const auto& req = recipe->resource_requirements.front();
            CHECK_EQ(req.kind, gtnh::common::ResourceKind::EU,
                     "rubber wood requirement is EU");
            CHECK_EQ(req.amount, uint32_t(32), "rubber wood EU amount");
            CHECK_EQ(req.tier, int16_t(0), "rubber wood tier");
        }
    }

    PASS();
}

// add-recipe-fluid-io: per-operation fluid inputs/outputs parse into the model
// and are validated against the canonical fluid id domain (1111:11:*).
static void test_recipe_manager_blast_furnace_domains() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    RecipeManager::RecipeManager mgr;
    CHECK(mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes"),
          "blast furnace recipes load");
    CHECK(mgr.loadMachinesFromYaml(DATA_DIR "/registry/machines.yaml"),
          "blast furnace machine classes load");

    const auto* ebf = mgr.getRecipeById("gtnh:ebf_iron_smelting");
    const auto* hbf = mgr.getRecipeById("gtnh:hbf_iron_smelting");
    CHECK(ebf != nullptr, "EBF iron recipe exists");
    CHECK(hbf != nullptr, "HBF iron recipe exists");
    if (ebf) {
        CHECK_EQ(ebf->energy_type, static_cast<uint8_t>(0),
                 "EBF recipe is EU-powered");
        CHECK_EQ(ebf->inputs[0].item_id, ItemId::pack("0:1110:001:26"),
                 "EBF consumes iron dust");
        CHECK_EQ(ebf->outputs[0].item_id, ItemId::pack("0:110:1"),
                 "EBF produces iron ingots");
        CHECK_EQ(ebf->outputs[0].count, uint8_t(2), "EBF produces two ingots");
        CHECK_EQ(ebf->resource_requirements.size(), size_t(0),
                 "EBF uses legacy EU debit without HU reservation");
    }
    if (hbf) {
        CHECK_EQ(hbf->energy_type, static_cast<uint8_t>(1),
                 "HBF recipe is HU-powered");
        CHECK_EQ(hbf->inputs[0].item_id, ItemId::pack("0:1110:001:26"),
                 "HBF consumes iron dust");
        CHECK_EQ(hbf->outputs[0].item_id, ItemId::pack("0:110:1"),
                 "HBF produces iron ingots");
        CHECK_EQ(hbf->outputs[0].count, uint8_t(2), "HBF produces two ingots");
        CHECK_EQ(hbf->resource_requirements.size(), size_t(0),
                 "HBF uses legacy HU debit without typed reservation");
    }
    PASS();
}

static void test_recipe_manager_fluid_io() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");

    const std::string valid =
        "class: furnace\n"
        "recipes:\n"
        "  - name: fluid_io_valid\n"
        "    inputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    outputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    duration: 100\n"
        "    fluid_inputs:\n"
        "      - { fluid: water, amount: 1000 }\n"
        "    fluid_outputs:\n"
        "      - { fluid: steam, amount: 250 }\n";
    RecipeManager::RecipeManager mgr;
    CHECK(mgr.loadRecipesFromYamlFile(makeTempFile(valid)), "fluid-io recipe loads");
    auto* r = mgr.getRecipeById("fluid_io_valid");
    CHECK(r != nullptr, "fluid-io recipe registered");
    if (r) {
        CHECK(r->hasFluidInputs(), "fluid_inputs present");
        CHECK(r->hasFluidOutputs(), "fluid_outputs present");
        CHECK(r->needsReservation(), "fluid inputs trigger reservation");
        CHECK_EQ(r->fluid_inputs.size(), size_t(1), "one fluid input");
        CHECK_EQ(r->fluid_inputs[0].fluid_id, ItemId::pack("1111:11:0"), "water resolved");
        CHECK_EQ(r->fluid_inputs[0].amount_mb, uint32_t(1000), "input mB kept");
        CHECK_EQ(r->fluid_outputs.size(), size_t(1), "one fluid output");
        CHECK_EQ(r->fluid_outputs[0].fluid_id, ItemId::pack("1111:11:1"), "steam resolved");
        CHECK_EQ(r->fluid_outputs[0].amount_mb, uint32_t(250), "output mB kept");
    }

    const std::string unknown_fluid =
        "class: furnace\n"
        "recipes:\n"
        "  - name: fluid_io_unknown\n"
        "    inputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    outputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    duration: 100\n"
        "    fluid_inputs:\n"
        "      - { fluid: not_a_fluid, amount: 100 }\n";
    CHECK(!mgr.loadRecipesFromYamlFile(makeTempFile(unknown_fluid)),
          "unknown fluid rejected");

    const std::string non_fluid =
        "class: furnace\n"
        "recipes:\n"
        "  - name: fluid_io_nonfluid\n"
        "    inputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    outputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    duration: 100\n"
        "    fluid_inputs:\n"
        "      - { fluid: \"0:11110:2\", amount: 100 }\n";
    CHECK(!mgr.loadRecipesFromYamlFile(makeTempFile(non_fluid)),
          "non-fluid item id rejected");

    const std::string zero_amount =
        "class: furnace\n"
        "recipes:\n"
        "  - name: fluid_io_zero\n"
        "    inputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    outputs:\n"
        "      - { item: 4, count: 1 }\n"
        "    duration: 100\n"
        "    fluid_inputs:\n"
        "      - { fluid: water, amount: 0 }\n";
    CHECK(!mgr.loadRecipesFromYamlFile(makeTempFile(zero_amount)),
          "zero amount rejected");

    PASS();
}

void test_recipe_manager() {
    TEST(recipe_manager_empty);
    TEST(recipe_manager_blast_furnace_domains);
    TEST(recipe_manager_load_crafting_table);
    TEST(recipe_manager_find_stick);
    TEST(recipe_manager_no_match);
    TEST(recipe_manager_item_id_formats);
    TEST(recipe_manager_craft_patterns);
    TEST(recipe_manager_unlock_era);
    TEST(recipe_manager_queries);
    TEST(recipe_manager_extractor_variants);
    TEST(recipe_manager_fluid_io);
}
