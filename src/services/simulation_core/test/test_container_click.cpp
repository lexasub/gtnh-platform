// Integration test: container session + click handler chain.
// Verifies that a session with correct slot count allows clicks through,
// and that an empty session (size=0) drops them (the cold-cache bug).
#include <libgtnh-net/test/test.h>

#include "Storage/InventoryClick.h"
#include "Storage/PlayerInventoryStore.h"
#include "Storage/ContainerSession.h"

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

#include <array>
#include <cstdint>

namespace {

using simcore::ContainerClick;
using simcore::InventoryRef;
using simcore::PersistSlot;
using simcore::kInventorySlots;
using simcore::ContainerSession;
using simcore::ContainerSessionRegistry;
using Slots = std::array<PersistSlot, kInventorySlots>;

PersistSlot stack(uint16_t id, uint8_t count, uint16_t meta = 0) {
    return PersistSlot{id, count, meta};
}

// ── Player-only clicks (container_id=0, no session needed) ─────────────────

void test_player_pickup_to_cursor() {
    Slots s{}; s[5] = stack(13, 64);
    PersistSlot cursor{};
    InventoryRef inv{&s, nullptr};
    ContainerClick c{};
    c.action_type = simcore::kActionClick;
    c.button = simcore::kButtonLeft;
    c.container_id = 0;
    c.slot = 5;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "pickup changes state");
    CHECK_EQ(cursor.item_id, uint16_t(13), "item in cursor");
    CHECK_EQ(cursor.count, uint8_t(64), "full stack");
    CHECK_EQ(s[5].item_id, uint16_t(0), "source emptied");
}

void test_player_place_from_cursor() {
    Slots s{};
    PersistSlot cursor = stack(7, 10);
    InventoryRef inv{&s, nullptr};
    ContainerClick c{};
    c.action_type = simcore::kActionClick;
    c.button = simcore::kButtonLeft;
    c.container_id = 0;
    c.slot = 3;
    CHECK(simcore::ApplyContainerClick(inv, cursor, c), "place changes state");
    CHECK_EQ(s[3].item_id, uint16_t(7), "item placed");
    CHECK_EQ(s[3].count, uint8_t(10), "full count");
    CHECK_EQ(cursor.item_id, uint16_t(0), "cursor emptied");
}

// ── Container clicks with a NORMAL session (slots pre-sized) ──────────────

void test_container_place_from_cursor() {
    // Player picks up item from slot 0
    Slots s{}; s[0] = stack(13, 5);
    PersistSlot cursor{};
    InventoryRef inv1{&s, nullptr};
    ContainerClick pick{};
    pick.action_type = simcore::kActionClick;
    pick.button = simcore::kButtonLeft;
    pick.container_id = 0;
    pick.slot = 0;
    CHECK(simcore::ApplyContainerClick(inv1, cursor, pick), "pickup");

    CHECK_EQ(cursor.item_id, uint16_t(13), "cursor has item after pickup");
    CHECK_EQ(s[0].item_id, uint16_t(0), "player slot emptied");

    // Place into container slot 0 — session is a machine with 4 slots.
    std::vector<PersistSlot> cont(4); // 4 pre-sized empty slots ← THE FIX
    InventoryRef inv2{&s, &cont};
    ContainerClick place{};
    place.action_type = simcore::kActionClick;
    place.button = simcore::kButtonLeft;
    place.container_id = 1;
    place.slot = 0;
    CHECK(simcore::ApplyContainerClick(inv2, cursor, place), "place into container");
    CHECK_EQ(cont[0].item_id, uint16_t(13), "item in container slot");
    CHECK_EQ(cont[0].count, uint8_t(5), "full count");
    CHECK_EQ(cursor.item_id, uint16_t(0), "cursor emptied");
}

void test_container_to_player() {
    Slots s{};
    std::vector<PersistSlot> cont(4);
    cont[0] = stack(9, 32);
    PersistSlot cursor{};

    // Pickup from container
    InventoryRef inv1{&s, &cont};
    ContainerClick pick{};
    pick.action_type = simcore::kActionClick;
    pick.button = simcore::kButtonLeft;
    pick.container_id = 1;
    pick.slot = 0;
    CHECK(simcore::ApplyContainerClick(inv1, cursor, pick), "pickup from container");
    CHECK_EQ(cursor.item_id, uint16_t(9), "cursor has container item");
    CHECK_EQ(cont[0].item_id, uint16_t(0), "container slot emptied");

    // Place into player inventory
    InventoryRef inv2{&s, &cont};
    ContainerClick place{};
    place.action_type = simcore::kActionClick;
    place.button = simcore::kButtonLeft;
    place.container_id = 0;
    place.slot = 10;
    CHECK(simcore::ApplyContainerClick(inv2, cursor, place), "place to player");
    CHECK_EQ(s[10].item_id, uint16_t(9), "item in player slot");
    CHECK_EQ(cursor.item_id, uint16_t(0), "cursor emptied");
}

void test_container_quickmove_to_player() {
    Slots s{}; s[15] = stack(13, 10); // pre-existing stack to merge onto
    std::vector<PersistSlot> cont(4);
    cont[0] = stack(13, 20);
    PersistSlot cursor{};
    InventoryRef inv{&s, &cont};

    ContainerClick qm{};
    qm.action_type = simcore::kActionQuickMove;
    qm.container_id = 1;
    qm.slot = 0;
    CHECK(simcore::ApplyContainerClick(inv, cursor, qm), "quick-move from container");
    CHECK_EQ(cont[0].item_id, uint16_t(0), "container source emptied");
    // Should merge onto existing stack at slot 15
    CHECK_EQ(s[15].count, uint8_t(30), "merged onto existing stack 10+20");
}

void test_container_drop_slot() {
    Slots s{};
    std::vector<PersistSlot> cont(4);
    cont[1] = stack(3, 5);
    PersistSlot cursor{};
    InventoryRef inv{&s, &cont};
    ContainerClick d{};
    d.action_type = simcore::kActionDrop;
    d.container_id = 1;
    d.slot = 1;
    CHECK(simcore::ApplyContainerClick(inv, cursor, d), "drop container slot");
    CHECK_EQ(cont[1].item_id, uint16_t(0), "container slot cleared");
}

// ── THE BUG: empty session (size=0) drops all clicks ─────────────────────

void test_empty_session_drops_click() {
    // Simulates a machine opened for the first time (cold cache):
    // session created with empty slots vector, async loadSlots
    // hasn't completed yet.
    Slots s{};
    std::vector<PersistSlot> cont; // size 0 — THE BUG
    PersistSlot cursor = stack(13, 5);
    InventoryRef inv{&s, &cont};

    ContainerClick click{};
    click.action_type = simcore::kActionClick;
    click.button = simcore::kButtonLeft;
    click.container_id = 1;
    click.slot = 0;

    // SlotAt returns nullptr because 0 >= cont.size() (0 >= 0).
    // ApplyClick gets tgt==nullptr → returns false → no mutation.
    bool changed = simcore::ApplyContainerClick(inv, cursor, click);
    CHECK(!changed, "BUG CONFIRMED: click on empty session is a silent no-op");
    CHECK_EQ(cursor.item_id, uint16_t(13), "cursor UNCHANGED — item stuck in hand");
    CHECK_EQ(cursor.count, uint8_t(5), "count unchanged");
}

void test_sized_session_works() {
    // Same scenario but with a properly sized session — THE FIX.
    Slots s{};
    std::vector<PersistSlot> cont(4); // 4 slots, properly sized
    PersistSlot cursor = stack(13, 5);
    InventoryRef inv{&s, &cont};

    ContainerClick click{};
    click.action_type = simcore::kActionClick;
    click.button = simcore::kButtonLeft;
    click.container_id = 1;
    click.slot = 0;

    bool changed = simcore::ApplyContainerClick(inv, cursor, click);
    CHECK(changed, "place into sized session works");
    CHECK_EQ(cont[0].item_id, uint16_t(13), "item placed in container");
    CHECK_EQ(cursor.item_id, uint16_t(0), "cursor emptied");
}

// ── ContainerSessionRegistry smoke test ──────────────────────────────────

void test_registry_open_find_close() {
    ContainerSessionRegistry reg;
    uint64_t pid = 42;

    ContainerSession* found = reg.find(pid);
    CHECK(found == nullptr, "no session before open");

    ContainerSession sess;
    sess.kind = ContainerSession::Kind::Machine;
    sess.x = 1; sess.y = 2; sess.z = 3;
    sess.entity_type = 5;
    sess.slots.assign(4, PersistSlot{});
    reg.open(pid, std::move(sess));

    found = reg.find(pid);
    CHECK(found != nullptr, "session found after open");
    CHECK_EQ(found->x, int32_t(1), "position preserved");
    CHECK_EQ(found->slots.size(), size_t(4), "slot count preserved");
    CHECK(found->slotsRef() != nullptr, "slotsRef non-null");
    CHECK_EQ(found->slotsRef()->size(), size_t(4), "slotsRef correct size");

    reg.close(pid);
    found = reg.find(pid);
    CHECK(found == nullptr, "session gone after close");
}

// ── Full session+click chain (the integration scenario) ──────────────────

void test_session_click_chain_player_to_container() {
    // Setup: player inventory, session registry, machine session with 4 slots
    ContainerSessionRegistry sessions;
    uint64_t pid = 1;

    ContainerSession sess;
    sess.kind = ContainerSession::Kind::Machine;
    sess.slots.assign(4, PersistSlot{}); // ← CORRECT: pre-sized
    sessions.open(pid, std::move(sess));

    Slots playerSlots{};
    playerSlots[3] = stack(7, 64);
    PersistSlot cursor{};

    // 1. Player picks up from own inventory (container_id=0)
    {
        InventoryRef inv{&playerSlots, nullptr};
        ContainerClick c{};
        c.action_type = simcore::kActionClick;
        c.button = simcore::kButtonLeft;
        c.container_id = 0;
        c.slot = 3;
        CHECK(simcore::ApplyContainerClick(inv, cursor, c), "step1: pickup");
        CHECK_EQ(cursor.item_id, uint16_t(7), "step1: cursor loaded");
    }

    // 2. Place into machine slot 2 (container_id=1)
    {
        ContainerSession* s = sessions.find(pid);
        CHECK(s != nullptr, "step2: session exists");
        auto* cont = s->slotsRef();
        CHECK(cont != nullptr, "step2: slotsRef non-null");
        CHECK_EQ(cont->size(), size_t(4), "step2: container has 4 slots");

        InventoryRef inv{&playerSlots, cont};
        ContainerClick c{};
        c.action_type = simcore::kActionClick;
        c.button = simcore::kButtonLeft;
        c.container_id = 1;
        c.slot = 2;
        CHECK(simcore::ApplyContainerClick(inv, cursor, c), "step2: place");
        CHECK_EQ((*cont)[2].item_id, uint16_t(7), "step2: machine slot has item");
        CHECK_EQ((*cont)[2].count, uint8_t(64), "step2: full stack");
        CHECK_EQ(cursor.item_id, uint16_t(0), "step2: cursor empty");
    }
}

} // namespace

void test_container_click() {
    TEST(player_pickup_to_cursor);
    TEST(player_place_from_cursor);
    TEST(container_place_from_cursor);
    TEST(container_to_player);
    TEST(container_quickmove_to_player);
    TEST(container_drop_slot);
    TEST(empty_session_drops_click);
    TEST(sized_session_works);
    TEST(registry_open_find_close);
    TEST(session_click_chain_player_to_container);
}
