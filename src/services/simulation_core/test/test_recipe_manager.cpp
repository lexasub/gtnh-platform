#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#include <recipe_manager_lib/RecipeManager.h>

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
    RecipeManager::RecipeManager mgr;
    mgr.loadRecipesFromYamlDirectory(DATA_DIR "/recipes");

    std::vector<RecipeManager::ItemStack> inputs = {
        {13, 1, 0},
        {13, 1, 0},
    };
    auto* recipe = mgr.findRecipeByInputs(14, inputs);  // crafting table
    if (recipe) {
        CHECK_EQ(recipe->id, std::string("base:stick"), "2 planks match stick recipe");
    }

    PASS();
}

static void test_recipe_manager_no_match() {
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

void test_recipe_manager() {
    TEST(recipe_manager_empty);
    TEST(recipe_manager_load_crafting_table);
    TEST(recipe_manager_find_stick);
    TEST(recipe_manager_no_match);
}
