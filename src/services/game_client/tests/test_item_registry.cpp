#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "Crafting/ClientItemRegistry.h"

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#define CHECK_NE(a, b, msg) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, msg)
#define PASS() do { ++g_passed; } while(0)

static void test_load_items_csv() {
    ItemRegistry::LoadFromCSV(DATA_DIR "/registry/items.csv");

    CHECK(ItemRegistry::GetItem(0) == nullptr, "item 0 (air) is skipped by design");

    auto* planks = ItemRegistry::GetItem(13);
    CHECK(planks != nullptr, "item 13 (oak_planks) exists");
    CHECK_EQ(planks->name, std::string("minecraft:oak_planks"), "oak_planks name");

    auto* coal = ItemRegistry::GetItem(44);
    CHECK(coal != nullptr, "item 44 (coal) exists");
    CHECK_EQ(coal->name, std::string("minecraft:coal"), "coal name");

    auto* unknown = ItemRegistry::GetItem(9999);
    CHECK(unknown == nullptr, "unknown item returns nullptr");

    auto all = ItemRegistry::GetAllItemIds();
    CHECK(!all.empty(), "GetAllItemIds returns non-empty");

    PASS();
}

static void test_item_properties() {
    ItemRegistry::LoadFromCSV(DATA_DIR "/registry/items.csv");

    auto* stone = ItemRegistry::GetItem(7);
    CHECK(stone != nullptr, "item 7 (stone) exists");
    CHECK_EQ(stone->stackSize, uint8_t(64), "stone stack size is 64");
    CHECK_EQ(stone->name, std::string("minecraft:cobblestone"), "cobblestone name");
    CHECK_EQ(ItemRegistry::GetStackSize(7), uint8_t(64), "GetStackSize for stone");

    auto* chest = ItemRegistry::GetItem(20);
    CHECK(chest != nullptr, "item 20 (chest) exists");
    CHECK_EQ(chest->name, std::string("minecraft:quartz"), "item 20 quartz name");

    PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

int main(int, char**) {
    printf("=== GameClient Item Registry Test ===\n\n");
    TEST(load_items_csv);
    TEST(item_properties);
    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
