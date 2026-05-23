#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "UI/Core/DragManager.h"

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
#define PASS() do { ++g_passed; } while(0)

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

static void test_idle_empty_slot() {
    std::vector<ItemStack> slots(5);
    DragManager dm;
    auto r = dm.OnSlotActivated(2, slots, 0, false);
    CHECK(!r.consumed, "empty slot not consumed");
    CHECK(!r.isDraggingAfter, "no drag");
    CHECK(!dm.IsDragging(), "not dragging");
    PASS();
}

static void test_idle_pickup() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    auto r = dm.OnSlotActivated(1, slots, 0, false);
    CHECK(r.consumed, "pickup consumed");
    CHECK(r.isDraggingAfter, "dragging after pickup");
    CHECK_EQ(r.sourceSlot, 1, "source slot");
    CHECK_EQ(r.item.item_id, 13, "item id");
    CHECK_EQ(r.item.count, 64, "item count");
    CHECK(dm.IsDragging(), "drag manager dragging");
    CHECK_EQ(dm.GetSourceSlot(), 1, "source slot");
    CHECK_EQ(dm.GetHeldItem().item_id, 13, "held item id");
    CHECK_EQ(slots[1].item_id, 0, "source slot empty");
    PASS();
}

static void test_hold_click_same_slot_cancels() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    dm.OnSlotActivated(1, slots, 0, false);
    CHECK(dm.IsDragging(), "now dragging");
    auto r = dm.OnSlotActivated(1, slots, 0, false);
    CHECK(r.consumed, "cancel consumed");
    CHECK(!dm.IsDragging(), "not dragging after cancel");
    CHECK_EQ(slots[1].item_id, 13, "item returned");
    CHECK_EQ(slots[1].count, 64, "full count returned");
    PASS();
}

static void test_drop_into_empty() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    dm.OnSlotActivated(1, slots, 0, false);
    auto r = dm.OnSlotActivated(3, slots, 0, false);
    CHECK(r.consumed, "drop consumed");
    CHECK(!dm.IsDragging(), "not dragging after drop");
    CHECK_EQ(r.targetSlot, 3, "target slot");
    CHECK_EQ(r.count, 64, "count");
    CHECK_EQ(slots[1].item_id, 0, "source emptied");
    CHECK_EQ(slots[3].item_id, 13, "target filled");
    CHECK_EQ(slots[3].count, 64, "target count");
    PASS();
}

static void test_merge_same_item() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 40, 0};
    slots[3] = {13, 20, 0};
    DragManager dm;
    dm.OnSlotActivated(1, slots, 0, false);
    auto r = dm.OnSlotActivated(3, slots, 0, false);
    CHECK(r.consumed, "merge consumed");
    CHECK(!dm.IsDragging(), "not dragging after merge");
    CHECK_EQ(slots[3].item_id, 13, "target item");
    CHECK_EQ(slots[3].count, 60, "merged count");
    CHECK_EQ(slots[1].item_id, 0, "source emptied");
    PASS();
}

static void test_swap_different_item() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    slots[3] = {7, 32, 0};
    DragManager dm;
    dm.OnSlotActivated(1, slots, 0, false);
    auto r = dm.OnSlotActivated(3, slots, 0, false);
    CHECK(r.consumed, "swap consumed");
    CHECK(!dm.IsDragging(), "not dragging after swap");
    CHECK_EQ(slots[3].item_id, 13, "target gets source item");
    CHECK_EQ(slots[1].item_id, 7, "source gets target item");
    CHECK_EQ(slots[1].count, 32, "source count correct");
    PASS();
}

static void test_cancel_returns_item() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    dm.OnSlotActivated(1, slots, 0, false);
    CHECK(dm.IsDragging(), "dragging");
    dm.CancelDrag(slots);
    CHECK(!dm.IsDragging(), "not dragging after cancel");
    CHECK_EQ(slots[1].item_id, 13, "item returned");
    CHECK_EQ(slots[1].count, 64, "full count returned");
    PASS();
}

static void test_reset_clears_state() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    dm.OnSlotActivated(1, slots, 0, false);
    CHECK(dm.IsDragging(), "dragging");
    dm.Reset();
    CHECK(!dm.IsDragging(), "not dragging after reset");
    CHECK_EQ(dm.GetSourceSlot(), -1, "source reset");
    CHECK_EQ(dm.GetHeldItem().item_id, 0, "held item reset");
    PASS();
}

static void test_sync_to_inventory_state() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    dm.OnSlotActivated(1, slots, 0, false);
    InventoryState inv;
    inv.slots.resize(5);
    dm.SyncTo(inv);
    CHECK(inv.isDragging, "isDragging synced");
    CHECK_EQ(inv.dragItem.item_id, 13, "dragItem synced");
    CHECK_EQ(inv.dragSourceSlot, 1, "source slot synced");
    PASS();
}

static void test_sync_from_inventory_state() {
    InventoryState inv;
    inv.slots.resize(5);
    inv.isDragging = true;
    inv.dragItem = {13, 64, 0};
    inv.dragSourceSlot = 1;

    DragManager dm;
    dm.SyncFrom(inv);
    CHECK(dm.IsDragging(), "dragging after sync");
    CHECK_EQ(dm.GetHeldItem().item_id, 13, "held item synced");
    CHECK_EQ(dm.GetSourceSlot(), 1, "source slot synced");
    PASS();
}

static void test_callback_fires_on_drop() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    int cbCalls = 0;
    uint8_t cbAction=0, cbSrc=0, cbTgt=0, cbCount=0;
    dm.SetActionCallback([&](uint8_t a, uint8_t s, uint8_t t, uint8_t c) {
        cbCalls++; cbAction=a; cbSrc=s; cbTgt=t; cbCount=c;
    });
    dm.OnSlotActivated(1, slots, 0, false);
    CHECK_EQ(cbCalls, 0, "no callback on pickup");
    dm.OnSlotActivated(3, slots, 0, false);
    CHECK_EQ(cbCalls, 1, "callback on drop");
    CHECK_EQ(cbAction, 0, "MOVE action type");
    CHECK_EQ(cbSrc, 1, "source slot");
    CHECK_EQ(cbTgt, 3, "target slot");
    CHECK_EQ(cbCount, 64, "count");
    PASS();
}

static void test_callback_fires_on_merge() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 40, 0};
    slots[3] = {13, 20, 0};
    DragManager dm;
    int cbCalls = 0;
    dm.SetActionCallback([&](uint8_t, uint8_t, uint8_t, uint8_t) { cbCalls++; });
    dm.OnSlotActivated(1, slots, 0, false);
    dm.OnSlotActivated(3, slots, 0, false);
    CHECK_EQ(cbCalls, 1, "callback on merge");
    PASS();
}

static void test_callback_fires_on_swap() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    slots[3] = {7, 32, 0};
    DragManager dm;
    int cbCalls = 0;
    dm.SetActionCallback([&](uint8_t, uint8_t, uint8_t, uint8_t) { cbCalls++; });
    dm.OnSlotActivated(1, slots, 0, false);
    dm.OnSlotActivated(3, slots, 0, false);
    CHECK_EQ(cbCalls, 1, "callback on swap");
    PASS();
}

static void test_no_callback_on_cancel() {
    std::vector<ItemStack> slots(5);
    slots[1] = {13, 64, 0};
    DragManager dm;
    int cbCalls = 0;
    dm.SetActionCallback([&](uint8_t, uint8_t, uint8_t, uint8_t) { cbCalls++; });
    dm.OnSlotActivated(1, slots, 0, false);
    dm.CancelDrag(slots);
    CHECK_EQ(cbCalls, 0, "no callback on cancel");
    PASS();
}

int main(int, char**) {
    printf("=== DragManager Test ===\n\n");
    TEST(idle_empty_slot);
    TEST(idle_pickup);
    TEST(hold_click_same_slot_cancels);
    TEST(drop_into_empty);
    TEST(merge_same_item);
    TEST(swap_different_item);
    TEST(cancel_returns_item);
    TEST(reset_clears_state);
    TEST(sync_to_inventory_state);
    TEST(sync_from_inventory_state);
    TEST(callback_fires_on_drop);
    TEST(callback_fires_on_merge);
    TEST(callback_fires_on_swap);
    TEST(no_callback_on_cancel);
    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
