#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "Crafting/ClientItemRegistry.h"
#include <common/ItemId.h>

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

    auto* planks = ItemRegistry::GetItem(ItemId::pack("0:10:0"));
    CHECK(planks != nullptr, "oak_planks exists");
    if (planks) {
        CHECK_EQ(planks->name, std::string("oak_planks"), "oak_planks name");
    }

    auto* coal = ItemRegistry::GetItem(ItemId::pack("0:11110:2"));
    CHECK(coal != nullptr, "coal exists");
    if (coal) {
        CHECK_EQ(coal->name, std::string("coal"), "coal name");
    }

    auto* unknown = ItemRegistry::GetItem(9999);
    CHECK(unknown == nullptr, "unknown item returns nullptr");

    auto all = ItemRegistry::GetAllItemIds();
    CHECK(!all.empty(), "GetAllItemIds returns non-empty");

    PASS();
}

static void test_component_subgroups() {
    ItemRegistry::LoadFromCSV(DATA_DIR "/registry/items.csv");
    struct Expected { const char* name; const char* id; };
    const Expected expected[] = {
        {"electronic_circuit_lv", "110:000:0"},
        {"integrated_circuit_mv", "110:000:1"},
        {"advanced_circuit_hv", "110:000:2"},
        {"processor_circuit_ev", "110:000:3"},
        {"circuit_board_empty", "110:001:0"},
        {"circuit_board_basic", "110:001:1"},
        {"fiberglass", "110:001:2"},
        {"conveyor_module", "110:010:0"},
        {"electric_motor", "110:010:1"},
        {"electric_piston", "110:010:2"},
        {"field_generator", "110:010:3"},
        {"fluid_pump", "110:010:4"},
        {"robot_arm", "110:010:5"},
        {"solenoid", "110:010:6"},
        {"capacitor_smd", "110:011:0"},
        {"coil_smd", "110:011:1"},
        {"diode_smd", "110:011:2"},
        {"flip_flop_smd", "110:011:3"},
        {"inductor_smd", "110:011:4"},
        {"logic_chip_smd", "110:011:5"},
        {"resistor_smd", "110:011:6"},
        {"transistor_smd", "110:011:7"},
        {"emitter", "110:100:0"},
        {"field_coil", "110:100:1"},
        {"sensor", "110:100:2"},
        {"resistor", "110:101:0"},
        {"capacitor", "110:101:1"},
        {"transistor", "110:101:2"},
        {"diode", "110:101:3"},
        {"inductor", "110:101:4"},
        {"coil", "110:101:5"},
    };
    for (const auto& item : expected) {
        const auto id = ItemId::pack(item.id);
        const auto* info = ItemRegistry::GetItem(id);
        CHECK(info != nullptr, item.name);
        if (info) {
            CHECK_EQ(info->name, std::string(item.name), "component name");
            CHECK_EQ(info->stackSize, uint8_t(64), "component stack size");
            CHECK_EQ(info->meta, uint16_t(0), "component metadata");
        }
    }
    PASS();
}

static void test_item_properties() {
    ItemRegistry::LoadFromCSV(DATA_DIR "/registry/items.csv");

    auto* stone = ItemRegistry::GetItem(ItemId::pack("0:0:2"));
    CHECK(stone != nullptr, "cobblestone exists");
    if (stone) {
        CHECK_EQ(stone->stackSize, uint8_t(64), "cobblestone stack size is 64");
        CHECK_EQ(stone->name, std::string("cobblestone"), "cobblestone name");
    }
    CHECK_EQ(ItemRegistry::GetStackSize(ItemId::pack("0:0:2")), uint8_t(64), "GetStackSize for cobblestone");

    auto* chest = ItemRegistry::GetItem(ItemId::pack("0:10:11:0"));
    CHECK(chest != nullptr, "chest exists");
    if (chest) {
        CHECK_EQ(chest->name, std::string("chest"), "chest name");
    }

    PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

int main(int, char**) {
    printf("=== GameClient Item Registry Test ===\n\n");
    TEST(load_items_csv);
    TEST(component_subgroups);
    TEST(item_properties);
    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
