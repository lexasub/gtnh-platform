// GameScenario / StartScenario flow tests:
// 7.1 protocol frame round-trip; 7.2 scenario table + apply ordering.
// 7.3 scenario-table loader contract: table invariants, cross-check of the
//     granted item ids against the real content registry, lookup semantics,
//     and the applyScenario failure paths (full inventory, no rollback).
#include <engine/net/test/test.h>

#include <game/scenario/GameScenario.h>
#include <game/storage/PlayerInventoryStore.h>
#include "core_generated.h"

#include <engine/registry/ItemId.h>

#include <flatbuffers/flatbuffers.h>
#include <cstdint>
#include <array>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef DATA_DIR
#error "DATA_DIR must be defined to the repository data/ root"
#endif

#ifndef TEST
#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)
#endif

// ---------------------------------------------------------------------------
// Helpers — the scenario table grants PACKED item ids; the content registry
// stores the same ids in hierarchical "prefix:prefix:payload" form. These
// helpers let the tests assert the table against the real data files instead of
// restating the table's own literals.
// ---------------------------------------------------------------------------
namespace {

std::set<uint16_t> loadRegistryItemIds() {
    std::set<uint16_t> ids;
    std::ifstream in(std::string(DATA_DIR) + "/registry/items.csv");
    if (!in) return ids;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        ids.insert(ItemId::pack(line.substr(0, comma)));
    }
    return ids;
}

int countItem(const simcore::PlayerInventoryStore& store, uint64_t pid, uint16_t id) {
    int total = 0;
    for (const auto& s : store.getSlots(pid)) {
        if (s.item_id == id) total += s.count;
    }
    return total;
}

bool hasItem(const simcore::PlayerInventoryStore& store, uint64_t pid, uint16_t id) {
    for (const auto& s : store.getSlots(pid)) {
        if (s.item_id == id && s.count > 0) return true;
    }
    return false;
}

int usedSlots(const simcore::PlayerInventoryStore& store, uint64_t pid) {
    int n = 0;
    for (const auto& s : store.getSlots(pid)) {
        if (s.item_id != 0) ++n;
    }
    return n;
}

simcore::GameScenario makeScenario(std::vector<std::pair<uint16_t, uint8_t>> items,
                                   bool clearFirst) {
    simcore::GameScenario sc;
    sc.index = 99;
    sc.name = "synthetic test scenario";
    sc.targetMode = static_cast<uint8_t>(Protocol::GameMode_CREATIVE);
    sc.giveItems = std::move(items);
    sc.clearFirst = clearFirst;
    sc.questBookEra = 3;
    return sc;
}

// Fill the first `n` slots to the max stack; leave the rest empty.
void fillSlots(simcore::PlayerInventoryStore& store, uint64_t pid, int n, uint16_t id) {
    std::array<simcore::PersistSlot, simcore::kInventorySlots> slots{};
    for (int i = 0; i < n && i < simcore::kInventorySlots; ++i) {
        slots[static_cast<size_t>(i)] = {id, 64, 0};
    }
    store.setSlots(pid, slots);
}

constexpr uint16_t kCraftingTable = 22529; // 0:10:11:1
constexpr uint16_t kWoodenPickaxe = 30723;  // 0:11110:3

} // namespace

static void test_StartScenarioReqFrame() {
  flatbuffers::FlatBufferBuilder fbb(64);
  auto req = Protocol::CreateStartScenarioReq(fbb, 1234, 0);
  fbb.Finish(req);

  flatbuffers::Verifier v(fbb.GetBufferPointer(), fbb.GetSize());
  CHECK(v.VerifyBuffer<Protocol::StartScenarioReq>(nullptr));
  auto* parsed =
      flatbuffers::GetRoot<Protocol::StartScenarioReq>(fbb.GetBufferPointer());
  CHECK(parsed != nullptr);
  CHECK_EQ(parsed->player_id(), uint64_t(1234));
  CHECK_EQ(parsed->scenario_index(), uint8_t(0));
}

static void test_StartScenarioRespFrame() {
  flatbuffers::FlatBufferBuilder fbb(64);
  auto err = fbb.CreateString("boom");
  auto resp = Protocol::CreateStartScenarioResp(
      fbb, 1234, 0, false, err, Protocol::GameMode_ADVENTURE, 2);
  fbb.Finish(resp);

  flatbuffers::Verifier v(fbb.GetBufferPointer(), fbb.GetSize());
  CHECK(v.VerifyBuffer<Protocol::StartScenarioResp>(nullptr));
  auto* parsed =
      flatbuffers::GetRoot<Protocol::StartScenarioResp>(fbb.GetBufferPointer());
  CHECK_EQ(parsed->player_id(), uint64_t(1234));
  CHECK_EQ(parsed->scenario_index(), uint8_t(0));
  CHECK_EQ(parsed->success(), false);
  CHECK_EQ(parsed->game_mode(), Protocol::GameMode_ADVENTURE);
  CHECK_EQ(parsed->quest_book_era(), uint8_t(2));
  CHECK(parsed->error() && parsed->error()->str() == "boom");
}

static void test_Scenario0Table() {
  const simcore::GameScenario* sc = simcore::findScenario(0);
  CHECK(sc != nullptr);
  CHECK_EQ(sc->targetMode, uint8_t(0));  // SURVIVAL
  CHECK(sc->clearFirst);
  CHECK_EQ(sc->questBookEra, uint8_t(0));
  CHECK_EQ(sc->giveItems.size(), size_t(2));
  if (sc && sc->giveItems.size() == 2) {
    CHECK_EQ(sc->giveItems[0].first, uint16_t(22529));  // crafting table
    CHECK_EQ(sc->giveItems[1].first, uint16_t(30723));  // wooden pickaxe
  }
}

static void test_ScenarioUnknownRejected() {
  CHECK(simcore::findScenario(1) == nullptr);
  CHECK(simcore::findScenario(255) == nullptr);
}

static void test_ApplyScenarioOrdering() {
  simcore::PlayerInventoryStore store;
  int postMutations = 0;
  store.setPostMutation(
      [&postMutations](uint64_t, const std::array<simcore::PersistSlot, simcore::kInventorySlots>&) {
        ++postMutations;
      });

  store.initPlayer(7);
  std::array<simcore::PersistSlot, simcore::kInventorySlots> prefilled{};
  prefilled[0].item_id = 999;
  prefilled[0].count = 1;
  store.setSlots(7, prefilled);

  const simcore::GameScenario* sc = simcore::findScenario(0);
  CHECK(sc != nullptr);
  if (!sc) return;

  int before = postMutations;
  CHECK(simcore::applyScenario(store, *sc, 7));
  CHECK_EQ(postMutations, before + 3);

  auto slotsAfter = store.getSlots(7);
  bool hasCraftTable = false, hasPickaxe = false, cleared = true;
  for (const auto& s : slotsAfter) {
    if (s.item_id == 22529 && s.count >= 1) hasCraftTable = true;
    if (s.item_id == 30723 && s.count >= 1) hasPickaxe = true;
    if (s.item_id == 999) cleared = false;  // pre-grant should be wiped
  }
  CHECK(hasCraftTable);
  CHECK(hasPickaxe);
  CHECK(cleared);
  CHECK_EQ(store.getGameMode(7), uint8_t(0));  // SURVIVAL
}

// ---------------------------------------------------------------------------
// 7.3 Scenario-table loader contract.
//
// FINDING (gp-1k5e): there is no file-backed scenario loader. `scenarios()`
// is a hard-coded `static const std::vector<GameScenario>` literal in
// GameScenario.cpp — no file, no parser, no DATA_DIR read. So the failure
// paths the issue asks about ("missing referenced world", "malformed
// scenario line") have no code path to exercise: they are structurally
// impossible today, not merely unvalidated. The tests below therefore pin the
// invariants a file-backed loader WOULD have to satisfy, and pin the
// behaviours that actually exist (unvalidated ids, no rollback on failure).
// ---------------------------------------------------------------------------

static void test_ScenarioTableInvariants() {
    const auto& all = simcore::scenarios();
    CHECK(!all.empty());

    std::set<uint8_t> seen;
    bool indicesAscending = true;
    uint8_t prev = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& sc = all[i];
        // Display name must be populated (the client prints it verbatim).
        CHECK(!sc.name.empty());
        // Unique index — findScenario() is a linear scan, duplicates would
        // make lookups ambiguous and the first match silently win.
        CHECK(seen.insert(sc.index).second);
        // targetMode must be a real GameMode the protocol defines.
        CHECK_GE(sc.targetMode, uint8_t(0));
        CHECK(static_cast<uint8_t>(sc.targetMode) <=
              static_cast<uint8_t>(Protocol::GameMode_SPECTATOR));
        // Granted stacks must be non-zero; a 0-count entry is a silent no-op.
        for (const auto& give : sc.giveItems) {
            CHECK_NE(give.second, uint8_t(0));
            CHECK_NE(give.first, uint16_t(0));
        }
        if (i > 0) indicesAscending = indicesAscending && (sc.index > prev);
        prev = sc.index;
    }
    CHECK(indicesAscending);
}

static void test_ScenarioItemsExistInRegistry() {
    // The strongest check available: every id the table grants must be a real
    // item in the shipped content registry. This is the "missing referenced
    // block" case for the one data the table does contain.
    const std::set<uint16_t> registry = loadRegistryItemIds();
    CHECK(!registry.empty());

    for (const auto& sc : simcore::scenarios()) {
        for (const auto& give : sc.giveItems) {
            CHECK(registry.count(give.first) == 1);
        }
    }
    // The two ids scenario 0 grants are the ones the comments name, and they
    // are the registry rows "0:10:11:1" (crafting_table) and "0:11110:3"
    // (wooden_pickaxe) once packed.
    CHECK_EQ(ItemId::pack("0:10:11:1"), kCraftingTable);
    CHECK_EQ(ItemId::pack("0:11110:3"), kWoodenPickaxe);
}

static void test_ScenarioTableIsStaticAndDeterministic() {
    // A file-backed loader would re-read per call; this one is a function-local
    // static. Assert the property so a future loader rewrite notices.
    const simcore::GameScenario* a = &simcore::scenarios().front();
    const simcore::GameScenario* b = &simcore::scenarios().front();
    CHECK(a == b);
    CHECK_EQ(simcore::scenarios().size(), size_t(1));
    // The returned reference is read-only by contract — the same contents on
    // every call (no lazily mutated state).
    CHECK_EQ(simcore::scenarios().front().giveItems.size(), size_t(2));
}

static void test_FindScenarioSweepsWholeIndexRange() {
    // 256 entries: exactly the in-range ones resolve, everything else nullptr.
    for (int i = 0; i <= 255; ++i) {
        const auto idx = static_cast<uint8_t>(i);
        const simcore::GameScenario* sc = simcore::findScenario(idx);
        bool inTable = false;
        for (const auto& e : simcore::scenarios()) {
            if (e.index == idx) inTable = true;
        }
        CHECK_EQ(sc != nullptr, inTable);
    }
    // Callers use a null result to reject the request; make sure that holds
    // for the boundaries most likely to slip through a signed comparison.
    CHECK(simcore::findScenario(0) != nullptr);
    CHECK(simcore::findScenario(1) == nullptr);
    CHECK(simcore::findScenario(254) == nullptr);
    CHECK(simcore::findScenario(255) == nullptr);
}

static void test_ApplyEmptyScenarioStillSetsGameMode() {
    // "Empty scenario": default-constructed — no items, no name, mode 0.
    // applyScenario has no guard for this; it succeeds and still flips mode.
    simcore::PlayerInventoryStore store;
    store.initPlayer(11);
    store.setGameMode(11, static_cast<uint8_t>(Protocol::GameMode_ADVENTURE));

    const simcore::GameScenario empty{};
    CHECK(empty.giveItems.empty());
    CHECK(simcore::applyScenario(store, empty, 11));
    // mode 0 == SURVIVAL — the default silently downgraded the player from
    // ADVENTURE. Nothing in applyScenario rejects an empty/mode-less scenario.
    CHECK_EQ(store.getGameMode(11), uint8_t(Protocol::GameMode_SURVIVAL));
    CHECK_EQ(usedSlots(store, 11), 0);
}

static void test_ApplyScenarioFullInventoryFails() {
    // The one real failure path: giveItem() returns false when the stack
    // cannot fit, and applyScenario propagates that false.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 12;
    store.initPlayer(pid);
    fillSlots(store, pid, simcore::kInventorySlots, /*id=*/1); // every slot full

    const auto sc = makeScenario({{kCraftingTable, 1}}, /*clearFirst=*/false);
    CHECK(!simcore::applyScenario(store, sc, pid));
    CHECK(!hasItem(store, pid, kCraftingTable));
}

static void test_ApplyScenarioPartialGrantIsNotRolledBack() {
    // Second grant fails (inventory full) → applyScenario returns false, but
    // the first grant is NOT undone. There is no rollback: a caller that only
    // checks the bool leaves the player half-scenario'd.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 13;
    store.initPlayer(pid);
    // Leave exactly one free slot so only the first of two items fits.
    fillSlots(store, pid, simcore::kInventorySlots - 1, /*id=*/1);

    const auto sc = makeScenario({{kCraftingTable, 1}, {kWoodenPickaxe, 1}},
                                 /*clearFirst=*/false);
    CHECK(!simcore::applyScenario(store, sc, pid));
    CHECK(hasItem(store, pid, kCraftingTable));
    CHECK(!hasItem(store, pid, kWoodenPickaxe));
    // Game mode is never reached on the failure branch.
    CHECK_NE(store.getGameMode(pid), static_cast<uint8_t>(Protocol::GameMode_CREATIVE));
}

static void test_ApplyScenarioClearFirstRescuesFullInventory() {
    // Same scenario as above but clearFirst=true: the wipe frees the slots, so
    // it succeeds. Documents that clearFirst is load-bearing, not cosmetic.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 14;
    store.initPlayer(pid);
    fillSlots(store, pid, simcore::kInventorySlots, /*id=*/1);

    const auto sc = makeScenario({{kCraftingTable, 1}}, /*clearFirst=*/true);
    CHECK(simcore::applyScenario(store, sc, pid));
    CHECK(hasItem(store, pid, kCraftingTable));
    CHECK_EQ(usedSlots(store, pid), 1);
    CHECK_EQ(store.getGameMode(pid), static_cast<uint8_t>(Protocol::GameMode_CREATIVE));
}

static void test_ApplyScenarioDoesNotValidateItemIds() {
    // FINDING: applyScenario grants whatever id it is handed. An id absent
    // from the registry is accepted silently — no lookup, no error, no log.
    // A scenario table that later becomes data-driven would have nowhere to
    // catch a typo'd or deleted item id.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 15;
    store.initPlayer(pid);

    // 0xDEAD is not a packed id for any items.csv row.
    const uint16_t bogus = 0xDEAD;
    const std::set<uint16_t> registry = loadRegistryItemIds();
    CHECK(registry.count(bogus) == 0);

    const auto sc = makeScenario({{bogus, 1}}, /*clearFirst=*/false);
    CHECK(simcore::applyScenario(store, sc, pid));
    CHECK(hasItem(store, pid, bogus));
}

static void test_ApplyScenarioZeroCountIsSilentNoOp() {
    // FINDING: a 0-count grant "succeeds" (giveItem with count 0 has nothing
    // left over, so it returns true) but places no item. The table invariant
    // test asserts no shipped scenario does this; a data-driven one could.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 16;
    store.initPlayer(pid);

    const auto sc = makeScenario({{kCraftingTable, 0}}, /*clearFirst=*/false);
    CHECK(simcore::applyScenario(store, sc, pid));
    CHECK_EQ(usedSlots(store, pid), 0);
}

static void test_ApplyScenarioStacksWithoutDuplicating() {
    // Applying twice with clearFirst=false must stack, not duplicate: the
    // second grant goes on top of the first in the same slot.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 17;
    store.initPlayer(pid);

    const auto sc = makeScenario({{kCraftingTable, 1}, {kWoodenPickaxe, 1}},
                                 /*clearFirst=*/false);
    CHECK(simcore::applyScenario(store, sc, pid));
    CHECK(simcore::applyScenario(store, sc, pid));
    CHECK_EQ(countItem(store, pid, kCraftingTable), 2);
    CHECK_EQ(countItem(store, pid, kWoodenPickaxe), 2);
    CHECK_EQ(usedSlots(store, pid), 2);
}

static void test_ApplyScenarioToUninitialisedPlayer() {
    // applyScenario never calls initPlayer; giveItem/setSlots default-construct
    // the map entry, so an un-initialised player is silently created.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 18;
    const auto sc = makeScenario({{kCraftingTable, 1}}, /*clearFirst=*/false);
    CHECK(simcore::applyScenario(store, sc, pid));
    CHECK(hasItem(store, pid, kCraftingTable));
}

static void test_ApplyScenarioIsolatesPlayers() {
    // Player state is keyed by id — scenario 0 applied to one player must not
    // disturb another.
    simcore::PlayerInventoryStore store;
    const auto* sc = simcore::findScenario(0);
    CHECK(sc != nullptr);
    if (!sc) return;

    std::array<simcore::PersistSlot, simcore::kInventorySlots> before{};
    before[0] = {999, 3, 0};
    store.setSlots(19, before);

    CHECK(simcore::applyScenario(store, *sc, 20));
    CHECK(simcore::applyScenario(store, *sc, 21));
    // Player 19 untouched by both applications.
    CHECK(hasItem(store, 19, 999));
    CHECK(!hasItem(store, 19, kCraftingTable));
    CHECK_EQ(store.getGameMode(20), static_cast<uint8_t>(Protocol::GameMode_SURVIVAL));
    CHECK_EQ(store.getGameMode(21), static_cast<uint8_t>(Protocol::GameMode_SURVIVAL));
}

static void test_ApplyScenarioPostMutationOrdering() {
    // Ordering guarantee the header documents and the simcore handler relies
    // on: every store call publishes synchronously, clearFirst first.
    simcore::PlayerInventoryStore store;
    const uint64_t pid = 22;
    store.initPlayer(pid);
    fillSlots(store, pid, simcore::kInventorySlots, /*id=*/1);

    std::vector<int> snapshotUsed;
    store.setPostMutation([&snapshotUsed](
                              uint64_t, const std::array<simcore::PersistSlot,
                                                         simcore::kInventorySlots>& slots) {
        int n = 0;
        for (const auto& s : slots) {
            if (s.item_id != 0) ++n;
        }
        snapshotUsed.push_back(n);
    });

    const auto sc = makeScenario({{kCraftingTable, 1}, {kWoodenPickaxe, 1}},
                                 /*clearFirst=*/true);
    CHECK(simcore::applyScenario(store, sc, pid));
    // One publish per store call: setSlots(empty) + one per giveItem.
    CHECK_EQ(snapshotUsed.size(), size_t(3));
    // The first published snapshot is the post-wipe empty inventory.
    CHECK_EQ(snapshotUsed[0], 0);
    CHECK_EQ(snapshotUsed[1], 1);
    CHECK_EQ(snapshotUsed[2], 2);
}

void test_game_scenario() {
    TEST(StartScenarioReqFrame);
    TEST(StartScenarioRespFrame);
    TEST(Scenario0Table);
    TEST(ScenarioUnknownRejected);
    TEST(ApplyScenarioOrdering);
    // 7.3 — loader contract + failure paths.
    TEST(ScenarioTableInvariants);
    TEST(ScenarioItemsExistInRegistry);
    TEST(ScenarioTableIsStaticAndDeterministic);
    TEST(FindScenarioSweepsWholeIndexRange);
    TEST(ApplyEmptyScenarioStillSetsGameMode);
    TEST(ApplyScenarioFullInventoryFails);
    TEST(ApplyScenarioPartialGrantIsNotRolledBack);
    TEST(ApplyScenarioClearFirstRescuesFullInventory);
    TEST(ApplyScenarioDoesNotValidateItemIds);
    TEST(ApplyScenarioZeroCountIsSilentNoOp);
    TEST(ApplyScenarioStacksWithoutDuplicating);
    TEST(ApplyScenarioToUninitialisedPlayer);
    TEST(ApplyScenarioIsolatesPlayers);
    TEST(ApplyScenarioPostMutationOrdering);
}
