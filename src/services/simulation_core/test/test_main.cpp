// simulation_core integration tests
// Uses the project-local test framework from libgtnh-net/test/test.h
#include <libgtnh-net/test/test.h>

#include "../InventoryActionHandler.h"

void test_ecs_systems();
void test_recipe_manager();
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <vector>
#include <type_traits>

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

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

// ---------------------------------------------------------------------------
// Test 1: ItemStack struct layout matches FlatBuffers schema
//   schema: struct ItemStack { item_id:uint16; count:uint8; meta:uint16; }
//   expected: 6 bytes total, 2+1+2 packing with 1 byte padding after count
// ---------------------------------------------------------------------------
void test_ItemStackLayout() {
    CHECK_EQ(sizeof(simulation_core::ItemStack), size_t(6),
             "ItemStack must be exactly 6 bytes (uint16+uint8+uint16 + padding)");

    simulation_core::ItemStack s;
    CHECK_EQ(s.item_id, uint16_t(0));
    CHECK_EQ(s.count,   uint8_t(0));
    CHECK_EQ(s.meta,    uint16_t(0));

    s.item_id = 42;
    s.count   = 16;
    s.meta    = 1;
    CHECK_EQ(s.item_id, uint16_t(42));
    CHECK_EQ(s.count,   uint8_t(16));
    CHECK_EQ(s.meta,    uint16_t(1));

    // Verify max values fit in declared types
    s.item_id = 0xFFFF;
    s.count   = 0xFF;
    s.meta    = 0xFFFF;
    CHECK_EQ(s.item_id, uint16_t(0xFFFF));
    CHECK_EQ(s.count,   uint8_t(0xFF));
    CHECK_EQ(s.meta,    uint16_t(0xFFFF));

    printf("    sizeof=%zu, item_id=%zu count=%zu meta=%zu\n",
           sizeof(simulation_core::ItemStack),
           offsetof(simulation_core::ItemStack, item_id),
           offsetof(simulation_core::ItemStack, count),
           offsetof(simulation_core::ItemStack, meta));
}

// ---------------------------------------------------------------------------
// Test 2: InventorySlot struct layout matches FlatBuffers InventorySlot table
//   schema: table InventorySlot { item_id:uint16; count:uint8; meta:uint16; }
//   C++ version adds slot_index:int16 for position tracking
// ---------------------------------------------------------------------------
void test_InventorySlotLayout() {
    CHECK_EQ(sizeof(simulation_core::InventorySlot), size_t(8),
             "InventorySlot must be 8 bytes (ItemStack 6 + int16_t slot_index)");

    simulation_core::InventorySlot s;
    CHECK_EQ(s.item_id,    uint16_t(0));
    CHECK_EQ(s.count,      uint8_t(0));
    CHECK_EQ(s.meta,       uint16_t(0));
    CHECK_EQ(s.slot_index, int16_t(0));

    s.item_id    = 100;
    s.count      = 32;
    s.meta       = 5;
    s.slot_index = 7;
    CHECK_EQ(s.item_id,    uint16_t(100));
    CHECK_EQ(s.count,      uint8_t(32));
    CHECK_EQ(s.meta,       uint16_t(5));
    CHECK_EQ(s.slot_index, int16_t(7));

    // InventorySlot is trivially copyable (important for serialization)
    CHECK(std::is_trivially_copyable_v<simulation_core::InventorySlot>);
}

// ---------------------------------------------------------------------------
// Test 3: Vec3i struct layout matches FlatBuffers Vec3i
//   schema: struct Vec3i { x:int32; y:int32; z:int32; }
//   expected: 12 bytes, 3×int32
// ---------------------------------------------------------------------------
void test_Vec3iLayout() {
    CHECK_EQ(sizeof(simulation_core::Vec3i), size_t(12),
             "Vec3i must be exactly 12 bytes (3 × int32)");

    simulation_core::Vec3i v;
    CHECK_EQ(v.x, 0); CHECK_EQ(v.y, 0); CHECK_EQ(v.z, 0);

    simulation_core::Vec3i v2(10, 20, 30);
    CHECK_EQ(v2.x, 10); CHECK_EQ(v2.y, 20); CHECK_EQ(v2.z, 30);

    // Verify max int32 range
    simulation_core::Vec3i v3(0x7FFFFFFF, -0x80000000, 0);
    CHECK_EQ(v3.x, 0x7FFFFFFF);
    CHECK_EQ(v3.y, static_cast<int32_t>(-0x80000000));
    CHECK_EQ(v3.z, 0);

    printf("    sizeof=%zu, x=%zu y=%zu z=%zu\n",
           sizeof(simulation_core::Vec3i),
           offsetof(simulation_core::Vec3i, x),
           offsetof(simulation_core::Vec3i, y),
           offsetof(simulation_core::Vec3i, z));
}

// ---------------------------------------------------------------------------
// Test 4: InventoryState default construction
// ---------------------------------------------------------------------------
void test_InventoryState() {
    simulation_core::InventoryState state{};
    CHECK_EQ(state.player_id, uint64_t(0));
    CHECK(state.slots.empty());

    simulation_core::InventoryState state2{42, {}};
    CHECK_EQ(state2.player_id, uint64_t(42));
    CHECK(state2.slots.empty());
}

// ---------------------------------------------------------------------------
// Test 5: InventorySlot aggregation: push_back + iteration
//   Tests the data structure used by all inventory operations
// ---------------------------------------------------------------------------
void test_InventorySlotsContainer() {
    std::vector<simulation_core::InventorySlot> slots;

    // Push 3 slots
    slots.push_back({100, 10, 1, 0});  // slot 0: 10 × item 100
    slots.push_back({200, 20, 2, 1});  // slot 1: 20 × item 200
    slots.push_back({300, 30, 3, 2});  // slot 2: 30 × item 300

    CHECK_EQ(slots.size(), size_t(3));

    CHECK_EQ(slots[0].item_id,    uint16_t(100));
    CHECK_EQ(slots[0].count,      uint8_t(10));
    CHECK_EQ(slots[0].meta,       uint16_t(1));
    CHECK_EQ(slots[0].slot_index, int16_t(0));

    CHECK_EQ(slots[1].item_id,    uint16_t(200));
    CHECK_EQ(slots[2].item_id,    uint16_t(300));

    // Erase middle slot
    slots.erase(slots.begin() + 1);
    CHECK_EQ(slots.size(), size_t(2));
    CHECK_EQ(slots[0].item_id, uint16_t(100));
    CHECK_EQ(slots[1].item_id, uint16_t(300));

    // Find by slot_index
    auto it = std::find_if(slots.begin(), slots.end(),
        [](const auto& s) { return s.slot_index == 0; });
    CHECK(it != slots.end());
    CHECK_EQ(it->item_id, uint16_t(100));

    // Modify in-place
    it->count -= 3;
    CHECK_EQ(it->count, uint8_t(7));
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("=== simulation_core test suite ===\n\n");

    TEST(ItemStackLayout);
    TEST(InventorySlotLayout);
    TEST(Vec3iLayout);
    TEST(InventoryState);
    TEST(InventorySlotsContainer);

    test_ecs_systems();
    test_recipe_manager();

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
