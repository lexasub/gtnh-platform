// Rule-table unit tests for the server-authoritative click model.
// Pure: exercises InventoryClick.h against in-memory arrays, no I/O.
#include <libgtnh-net/test/test.h>

#include "Storage/InventoryClick.h"
#include "Storage/PlayerInventoryStore.h"

// TEST is defined per-TU (test_main.cpp owns the runner); mirror it here.
#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

#include <array>
#include <cstdint>

namespace {

using simcore::ContainerClick;
using simcore::InventoryRef;
using simcore::PersistSlot;
using simcore::kInventorySlots;
using Slots = std::array<PersistSlot, kInventorySlots>;

PersistSlot stack(uint16_t id, uint8_t count, uint16_t meta = 0) {
    return PersistSlot{id, count, meta};
}

void test_pickup_lmb_empty_cursor() {
    Slots s{}; s[3] = stack(13, 64);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionClick; c.button = simcore::kButtonLeft; c.slot = 3;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "pickup changes state");
    CHECK_EQ(cursor.item_id, uint16_t(13), "cursor holds item");
    CHECK_EQ(cursor.count, uint8_t(64), "full stack to cursor");
    CHECK(s[3].item_id == 0, "source emptied");
}

void test_pickup_empty_cursor_empty_slot_noop() {
    Slots s{};
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionClick; c.button = simcore::kButtonLeft; c.slot = 0;
    CHECK(!simcore::ApplyContainerClick(inv, cursor, c), "no change on empty");
}

void test_place_lmb_into_empty() {
    Slots s{}; s[1] = stack(7, 32);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick pick{}; pick.action_type = simcore::kActionClick; pick.button = simcore::kButtonLeft; pick.slot = 1;
    CHECK(simcore::ApplyContainerClick(inv, cursor, pick), "pickup");
    ContainerClick place{}; place.action_type = simcore::kActionClick; place.button = simcore::kButtonLeft; place.slot = 5;
    CHECK(simcore::ApplyContainerClick(inv, cursor, place), "place");
    CHECK(s[5].item_id == 7 && s[5].count == 32, "target filled");
    CHECK(cursor.item_id == 0, "cursor emptied");
}

void test_merge_same_item() {
    Slots s{}; s[1] = stack(13, 40); s[3] = stack(13, 20);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick pick{}; pick.action_type = simcore::kActionClick; pick.button = simcore::kButtonLeft; pick.slot = 1;
    simcore::ApplyContainerClick(inv, cursor, pick);
    ContainerClick merge{}; merge.action_type = simcore::kActionClick; merge.button = simcore::kButtonLeft; merge.slot = 3;
    CHECK(simcore::ApplyContainerClick(inv, cursor, merge), "merge changes state");
    CHECK_EQ(s[3].count, uint8_t(60), "merged count");
    CHECK(cursor.item_id == 0, "cursor emptied");
}

void test_swap_different_item() {
    Slots s{}; s[1] = stack(13, 64); s[3] = stack(7, 32);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick pick{}; pick.action_type = simcore::kActionClick; pick.button = simcore::kButtonLeft; pick.slot = 1;
    simcore::ApplyContainerClick(inv, cursor, pick);
    ContainerClick swp{}; swp.action_type = simcore::kActionClick; swp.button = simcore::kButtonLeft; swp.slot = 3;
    CHECK(simcore::ApplyContainerClick(inv, cursor, swp), "swap changes state");
    CHECK_EQ(s[3].item_id, uint16_t(13), "target gets cursor item");
    CHECK_EQ(cursor.item_id, uint16_t(7), "cursor holds target item");
    CHECK(s[1].item_id == 0, "source stays empty (server model)");
}

void test_rmb_takes_half() {
    Slots s{}; s[2] = stack(9, 5);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionClick; c.button = simcore::kButtonRight; c.slot = 2;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "half changes state");
    CHECK_EQ(cursor.count, uint8_t(3), "ceil(5/2)=3 to cursor");
    CHECK_EQ(s[2].count, uint8_t(2), "remainder stays");
    // single item: right-click takes 1
    ContainerClick c2{}; c2.action_type = simcore::kActionClick; c2.button = simcore::kButtonRight; c2.slot = 2;
    PersistSlot cur2{}; Slots s2{}; s2[2] = stack(9, 1);
    InventoryRef inv2{&s2, nullptr};
    CHECK(simcore::ApplyContainerClick(inv2, cur2, c2), "half of 1");
    CHECK_EQ(cur2.count, uint8_t(1), "ceil(1/2)=1");
    CHECK(s2[2].item_id == 0, "slot emptied");
}

void test_rmb_places_one() {
    Slots s{};
    PersistSlot cursor = stack(5, 10);
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionClick; c.button = simcore::kButtonRight; c.slot = 0;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "place 1");
    CHECK_EQ(s[0].count, uint8_t(1), "one placed");
    CHECK_EQ(cursor.count, uint8_t(9), "cursor decremented");
}

void test_rmb_different_item_noop() {
    Slots s{}; s[0] = stack(5, 1);
    PersistSlot cursor = stack(9, 5);
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionClick; c.button = simcore::kButtonRight; c.slot = 0;
    CHECK(!simcore::ApplyContainerClick(inv, cursor, c), "different item no-op");
}

void test_quick_move_hotbar_to_main() {
    Slots s{}; s[2] = stack(13, 64);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionQuickMove; c.slot = 2; // hotbar
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "quick-move changes state");
    CHECK(s[2].item_id == 0, "hotbar emptied");
    bool found = false;
    for (int i = simcore::kHotbarCount; i < kInventorySlots; ++i) {
        if (s[i].item_id == 13) { found = true; break; }
    }
    CHECK(found, "stack moved to main");
}

void test_quick_move_stacks_onto_same_item() {
    Slots s{}; s[2] = stack(13, 10); s[15] = stack(13, 60);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionQuickMove; c.slot = 2;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "quick-move stacks");
    CHECK_EQ(s[15].count, uint8_t(64), "stacked to 64");
    CHECK(s[2].item_id == 0, "source cleared");
    // Remainder lands in the first empty main slot (slot 10), stacking-first.
    CHECK_EQ(s[10].item_id, uint16_t(13), "remainder to first empty main slot");
    CHECK_EQ(s[10].count, uint8_t(6), "remainder count");
}

void test_drop_cursor() {
    Slots s{};
    PersistSlot cursor = stack(3, 4);
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionDrop; c.slot = 0;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "drop cursor");
    CHECK(cursor.item_id == 0, "cursor cleared");
}

void test_drop_slot_when_cursor_empty() {
    Slots s{}; s[4] = stack(3, 4);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionDrop; c.slot = 4;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "drop slot");
    CHECK(s[4].item_id == 0, "slot cleared");
}

void test_drag_place() {
    Slots s{};
    PersistSlot cursor = stack(5, 10);
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionDragPlace; c.slot = 7;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "drag place");
    CHECK_EQ(s[7].count, uint8_t(1), "one placed");
    CHECK_EQ(cursor.count, uint8_t(9), "cursor decremented");
}

void test_pickup_all() {
    Slots s{}; s[0] = stack(5, 10); s[12] = stack(5, 20); s[30] = stack(9, 1);
    PersistSlot cursor = stack(5, 5);
    InventoryRef inv{&s, nullptr};
    ContainerClick c{}; c.action_type = simcore::kActionPickupAll; c.slot = 0;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "pickup-all changes state");
    CHECK_EQ(cursor.count, uint8_t(35), "all matching collected");
    CHECK(s[0].item_id == 0 && s[12].item_id == 0, "matching sources emptied");
    CHECK_EQ(s[30].item_id, uint16_t(9), "different item untouched");
}

void test_meta_preserved_on_merge() {
    Slots s{}; s[1] = stack(2, 10, 7); s[3] = stack(2, 20, 7);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick pick{}; pick.action_type = simcore::kActionClick; pick.button = simcore::kButtonLeft; pick.slot = 1;
    simcore::ApplyContainerClick(inv, cursor, pick);
    ContainerClick merge{}; merge.action_type = simcore::kActionClick; merge.button = simcore::kButtonLeft; merge.slot = 3;
    simcore::ApplyContainerClick(inv, cursor, merge);
    CHECK_EQ(s[3].count, uint8_t(30), "merged count");
    CHECK_EQ(s[3].meta, uint16_t(7), "meta preserved");
}

} // namespace

void test_inventory_click() {
    TEST(pickup_lmb_empty_cursor);
    TEST(pickup_empty_cursor_empty_slot_noop);
    TEST(place_lmb_into_empty);
    TEST(merge_same_item);
    TEST(swap_different_item);
    TEST(rmb_takes_half);
    TEST(rmb_places_one);
    TEST(rmb_different_item_noop);
    TEST(quick_move_hotbar_to_main);
    TEST(quick_move_stacks_onto_same_item);
    TEST(drop_cursor);
    TEST(drop_slot_when_cursor_empty);
    TEST(drag_place);
    TEST(pickup_all);
    TEST(meta_preserved_on_merge);
}
