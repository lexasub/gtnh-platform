// InventoryLoadHandler + PlayerJoinedHandler unit tests (issue gp-qxbu).
//
// Files under test:
//   src/game/storage/InventoryLoadHandler.cpp  (22 lines) — the
//       "player.inventory.load" topic: MetaDB's saved inventory arrives as a
//       Protocol::InventoryUpdate FlatBuffer and is written into the store,
//   src/game/storage/PlayerJoinedHandler.cpp    (32 lines) — the
//       "player.joined" topic: the player-join bootstrap that seeds the
//       inventory and the quest state and asks MetaDB to restore quest
//       progress.
//
// This is the multiplayer-foundation join path (add-multiplayer-foundation
// 2.2/6.4). The join sequence it implements is:
//
//   player.joined            -> initPlayer + quest seed + quest.get request
//   player.inventory.load    -> applyUpdate + republish player.inventory.update
//   meta_db.quest.get.response-> loadProgress
//
// so a joining player is EMPTY on entry and filled by the load that follows.
// "Joining fresh yields an empty inventory, not a stale one" is therefore the
// load-ordering contract, and the majority of these cases pin it.
//
// DETERMINISM AND ISOLATION
// -------------------------
// The router client is CONSTRUCTED BUT NEVER CONNECTED. RouterClient::publish
// returns immediately while disconnected (src/engine/net/src/router_client.cpp:
// 115,121), so Publish()/PublishRaw() are genuine no-ops: no socket, no thread,
// no cluster, no MessageRouter, no wall-clock, no sleeps. What the handlers
// publish is observed through the store they mutate, and through the
// QuestManager's own recording publisher — the same seam the QuestManager
// suite uses (test_questmanager_gating.cpp:110-125).
//
// Single-player semantics only (player 1) per the issue, plus one second player
// to prove the per-player keying of the bootstrap.
//
// Uses the PROJECT's own CHECK/TEST harness (src/engine/net/test/test.h
// convention, mirrored by src/game/machines/test/test_explosion_system.cpp).
// GoogleTest is deliberately NOT used: it is absent from conanfile.txt, CI does
// not install libgtest-dev, and CI builds Release with a global -Werror.

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <asio.hpp>

#include "core_generated.h"
#include "quest_generated.h"

#include <game/storage/InventoryLoadHandler.h>
#include <game/storage/PlayerJoinedHandler.h>
#include <game/storage/PlayerInventoryStore.h>
#include <apps/simcore/Network/ITopicHandler.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include <game/quests/QuestData.h>
#include <game/quests/QuestGraph.h>
#include <game/quests/QuestManager.h>

// ---------------------------------------------------------------------------
// Harness — definitions and macros near the TOP, before first use.
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "",
           msg ? msg : "");
  }
}

#define CHECK(cond, ...) \
  test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) \
  test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) \
  test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define TEST(name) \
  do {               \
    printf("  TEST: %s\n", #name); \
    test_##name();   \
  } while (0)

namespace {

// Cast helper so CHECK_EQ never compares a signed and an unsigned operand
// (which -Wextra/-Werror would reject at the macro expansion site).
constexpr int i_(size_t v) { return static_cast<int>(v); }
constexpr int i_(int v) { return v; }
constexpr int i_(uint8_t v) { return static_cast<int>(v); }
constexpr int i_(uint16_t v) { return static_cast<int>(v); }
constexpr int i_(uint32_t v) { return static_cast<int>(v); }

constexpr int kSlots = simcore::kInventorySlots;
constexpr uint8_t kMaxStack = 64;
constexpr uint64_t kPlayer = 1;  // single-player semantics (gp-qxbu)
constexpr uint64_t kOtherPlayer = 7;
constexpr uint16_t kItemA = 1000;
constexpr uint16_t kItemB = 1001;
constexpr uint16_t kItemC = 1002;

using Slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>;

simcore::PersistSlot item(uint16_t id, uint8_t count, uint16_t meta = 0) {
  return simcore::PersistSlot{id, count, meta};
}

int totalOf(const Slots& s, uint16_t id) {
  int n = 0;
  for (const auto& sl : s) {
    if (sl.item_id == id) n += sl.count;
  }
  return n;
}

int occupiedSlots(const Slots& s) {
  int n = 0;
  for (const auto& sl : s) {
    if (sl.item_id != 0) ++n;
  }
  return n;
}

bool sameSlot(const simcore::PersistSlot& a, const simcore::PersistSlot& b) {
  return a.item_id == b.item_id && a.count == b.count && a.meta == b.meta;
}

bool sameSlots(const Slots& a, const Slots& b) {
  for (size_t k = 0; k < a.size(); ++k) {
    if (!sameSlot(a[k], b[k])) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The topics the bootstrap uses, pinned by name so a rename is a visible
// failure rather than a silently unexercised publish path.
// ---------------------------------------------------------------------------
constexpr const char* kTopicJoined = "player.joined";
constexpr const char* kTopicInventoryLoad = "player.inventory.load";
constexpr const char* kTopicInventoryUpdate = "player.inventory.update";
constexpr const char* kTopicQuestGet = "meta_db.quest.get";

// The two handler topics as SimCoreMessageHandler registers them
// (src/apps/simcore/Network/SimCoreMessageHandler.cpp:145,149). Asserted so a
// rename in the dispatcher shows up here.
constexpr const char* kRegisteredLoadTopic = "player.inventory.load";
constexpr const char* kRegisteredJoinedTopic = "player.joined";

// ---------------------------------------------------------------------------
// The router client, deliberately never connected.
// ---------------------------------------------------------------------------
struct Router {
  // Constructed, never Connect()ed: every Publish short-circuits on
  // `!connected_`, so nothing reaches a socket.
  std::shared_ptr<simcore::IoUringRouterClient> client =
      std::make_shared<simcore::IoUringRouterClient>();

  Router() { CHECK(!client->IsConnected(), "the test router is offline by construction"); }
};

// A QuestManager publisher that records instead of sending. The join handler
// never calls the publish callback itself — only QuestManager does — so this
// transcript is exactly the quest traffic one join produced.
struct PublishedMsg {
  std::string topic;
  std::vector<uint8_t> data;
};

struct RecordingPublisher {
  std::vector<PublishedMsg> msgs;
  std::function<void(const std::string&, const uint8_t*, size_t)> callback() {
    return [this](const std::string& topic, const uint8_t* data, size_t len) {
      msgs.push_back(PublishedMsg{topic, std::vector<uint8_t>(data, data + len)});
    };
  }
  int countTopic(const std::string& topic) const {
    int n = 0;
    for (const auto& m : msgs) {
      if (m.topic == topic) ++n;
    }
    return n;
  }
  void clear() { msgs.clear(); }
};

// ---------------------------------------------------------------------------
// Wire builders — the exact FlatBuffers main.cpp / MetaDB produce.
// ---------------------------------------------------------------------------

// "player.joined" payload: Protocol::PlayerJoined { player_id }.
std::vector<uint8_t> joinedPayload(uint64_t pid) {
  flatbuffers::FlatBufferBuilder fb(64);
  fb.Finish(Protocol::CreatePlayerJoined(fb, pid));
  return std::vector<uint8_t>(fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize());
}

// "player.inventory.load" payload: an InventoryUpdate carrying the saved rows.
// MetaDB sends only player_id + slots (InventoryLoadHandler.cpp:11 reads those
// two and nothing else). `n` is how many leading slots the save carries, so a
// case can pin a save shorter than the 40-slot grid.
std::vector<uint8_t> inventoryLoadPayload(uint64_t pid, const Slots& slots,
                                          size_t n = static_cast<size_t>(kSlots)) {
  flatbuffers::FlatBufferBuilder fb(1024);
  std::vector<flatbuffers::Offset<Protocol::InventorySlot>> fbSlots;
  fbSlots.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    fbSlots.push_back(Protocol::CreateInventorySlot(fb, slots[i].item_id,
                                                    slots[i].count, slots[i].meta));
  }
  auto slotsVec = fb.CreateVector(fbSlots);
  auto off = Protocol::CreateInventoryUpdate(fb, pid, slotsVec);
  fb.Finish(off);
  return std::vector<uint8_t>(fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize());
}

// A structurally valid InventoryUpdate with an EMPTY slot vector.
std::vector<uint8_t> emptyInventoryLoadPayload(uint64_t pid) {
  return inventoryLoadPayload(pid, Slots{}, 0);
}

// The rig: an offline router plus (optionally) a recording quest publisher.
// The store is a plain value the cases own, wrapped in a non-owning
// shared_ptr because both handlers take one.
struct Rig {
  Router router;
  RecordingPublisher publisher;

  // A shared_ptr the handlers accept that does NOT own the store.
  static std::shared_ptr<simcore::PlayerInventoryStore> borrow(
      simcore::PlayerInventoryStore& store) {
    return std::shared_ptr<simcore::PlayerInventoryStore>(
        &store, [](simcore::PlayerInventoryStore*) {});
  }
};

} // namespace

// ---------------------------------------------------------------------------
// The topics are the ones SimCore actually subscribes to
// ---------------------------------------------------------------------------
static void test_the_bootstrap_topics_are_the_registered_ones() {
  // SimCoreMessageHandler.cpp:145 registers InventoryLoadHandler on
  // "player.inventory.load" and :149 registers PlayerJoinedHandler on
  // "player.joined". If either string is renamed the handler becomes dead
  // code while still compiling and passing, so pin the names.
  CHECK_EQ(std::string(kTopicInventoryLoad), std::string(kRegisteredLoadTopic),
           "the load handler is registered on player.inventory.load");
  CHECK_EQ(std::string(kTopicJoined), std::string(kRegisteredJoinedTopic),
           "the join handler is registered on player.joined");
  // main.cpp:589 subscribes the load topic and messageHandler.subscribeAll()
  // covers the join topic; the outbound topics the handlers publish are these.
  CHECK_EQ(std::string(kTopicInventoryUpdate), std::string("player.inventory.update"),
           "the load handler republishes on player.inventory.update");
  CHECK_EQ(std::string(kTopicQuestGet), std::string("meta_db.quest.get"),
           "the join handler requests quest progress on meta_db.quest.get");
}

// ---------------------------------------------------------------------------
// Joining with an EXISTING save: the load restores it
// ---------------------------------------------------------------------------
static void test_joining_with_an_existing_save_loads_that_inventory() {
  Rig rig;
  simcore::PlayerInventoryStore existing;
  simcore::InventoryLoadHandler handler(Rig::borrow(existing), rig.router.client);

  Slots saved{};
  saved[0] = item(kItemA, 12);
  saved[1] = item(kItemB, 64);
  saved[2] = item(kItemA, 5);
  saved[39] = item(kItemB, 3, 7);
  existing.setSlots(kPlayer, saved);

  // A fresh store, a real load, the restored state.
  simcore::PlayerInventoryStore fresh;
  simcore::InventoryLoadHandler loadHandler(Rig::borrow(fresh), rig.router.client);
  CHECK(occupiedSlots(fresh.getSlots(kPlayer)) == 0, "the fresh store is empty");
  loadHandler.handle(inventoryLoadPayload(kPlayer, saved));

  const Slots after = fresh.getSlots(kPlayer);
  CHECK(sameSlots(after, saved),
        "OBSERVED: the load applies only the 40 slots it parsed, and the store "
        "starts zeroed, so the whole array round-trips byte for byte");
  CHECK_EQ(i_(after[0].item_id), i_(kItemA), "slot 0 restored");
  CHECK_EQ(i_(after[0].count), 12, "with its count");
  CHECK_EQ(i_(after[1].count), 64, "a full stack is restored intact");
  CHECK_EQ(i_(after[2].item_id), i_(kItemA), "a split second stack is restored");
  CHECK_EQ(i_(after[2].count), 5, "with its own count");
  CHECK_EQ(i_(after[39].item_id), i_(kItemB), "the last slot is restored");
  CHECK_EQ(i_(after[39].meta), 7, "including its meta");
  CHECK_EQ(totalOf(after, kItemA), 17, "the restored total is 12 + 5");
  // The store already held the same save, so a reload must be idempotent.
  handler.handle(inventoryLoadPayload(kPlayer, saved));
  CHECK(sameSlots(existing.getSlots(kPlayer), saved),
        "a second load of the same save changes nothing");
}

// ---------------------------------------------------------------------------
// Joining fresh: EMPTY, not stale
// ---------------------------------------------------------------------------
static void test_joining_fresh_yields_an_empty_inventory_not_a_stale_one() {
  Rig rig;
  simcore::PlayerInventoryStore fresh;
  auto freshPtr = Rig::borrow(fresh);
  simcore::InventoryLoadHandler loadHandler(freshPtr, rig.router.client);
  simcore::PlayerJoinedHandler joinHandler(freshPtr, rig.router.client, nullptr);

  // A join with no preceding load must leave a brand-new player EMPTY.
  joinHandler.handle(joinedPayload(kPlayer));
  const Slots afterJoin = fresh.getSlots(kPlayer);
  CHECK(occupiedSlots(afterJoin) == 0,
        "OBSERVED: initPlayer leaves 40 empty slots — it never copies another "
        "player's state, and a fresh player has no save to inherit");
  CHECK(sameSlots(afterJoin, Slots{}), "every slot is the zeroed default");

  // The load that DOES arrive — an explicitly empty save — still leaves it empty.
  loadHandler.handle(emptyInventoryLoadPayload(kPlayer));
  CHECK(sameSlots(fresh.getSlots(kPlayer), Slots{}),
        "an empty save leaves the inventory empty rather than half-applied");
  CHECK_EQ(occupiedSlots(fresh.getSlots(kPlayer)), 0, "still 40 empty slots");
}

static void test_a_fresh_join_does_not_inherit_another_players_inventory() {
  Rig rig;
  simcore::PlayerInventoryStore store;
  simcore::PlayerJoinedHandler joinHandler(Rig::borrow(store), rig.router.client, nullptr);

  // Player 1 has a full inventory...
  Slots a{};
  a[0] = item(kItemA, 64);
  a[1] = item(kItemB, 30);
  store.setSlots(kPlayer, a);

  // ...and player 7 joins. initPlayer is per-id (unordered_map keyed by
  // player_id), so 7 must start empty.
  joinHandler.handle(joinedPayload(kOtherPlayer));
  CHECK(occupiedSlots(store.getSlots(kOtherPlayer)) == 0,
        "the second player starts empty");
  CHECK(sameSlots(store.getSlots(kPlayer), a),
        "and player 1's inventory is untouched by the second join");
  CHECK_EQ(totalOf(store.getSlots(kPlayer), kItemA), 64, "player 1 still holds 64");
}

// ---------------------------------------------------------------------------
// The join publishes its quest request exactly once
// ---------------------------------------------------------------------------
static void test_the_join_handler_publishes_once_per_join() {
  Rig rig;
  simcore::PlayerInventoryStore store;
  simcore::PlayerJoinedHandler joinHandler(Rig::borrow(store), rig.router.client, nullptr);

  // One join -> one initPlayer. The QuestManager is null here, so the only
  // observable side effect is the store; the join must be repeatable and
  // idempotent (a rejoin is the same message shape).
  joinHandler.handle(joinedPayload(kPlayer));
  joinHandler.handle(joinedPayload(kPlayer));
  joinHandler.handle(joinedPayload(kPlayer));
  const Slots after = store.getSlots(kPlayer);
  CHECK(occupiedSlots(after) == 0, "three joins leave the player empty");
  CHECK(sameSlots(after, Slots{}), "and byte-identical to a single join");

  // The player ids in the three messages were honoured individually.
  joinHandler.handle(joinedPayload(kOtherPlayer));
  CHECK(occupiedSlots(store.getSlots(kOtherPlayer)) == 0,
        "a different player id is a different, also-empty, entry");
}

static void test_the_join_handler_seeds_the_quest_manager() {
  // The quest half of the bootstrap: PlayerJoinedHandler::handle calls
  // QuestManager::onPlayerJoined(pid), which seeds root quests AVAILABLE and
  // dependents LOCKED, and the join is rejected without a manager.
  // Synthetic in-memory quest set — no real data file is read.
  quest::QuestData qd;
  quest::QuestGraph qg;

  // A 4-quest in-memory graph: 1 root; 2 and 3 depend on 1; 4 depends on 2.
  const std::unordered_map<uint32_t, std::vector<uint32_t>> graph{
      {1, {2, 3}}, {2, {4}}, {3, {}}, {4, {}}};
  std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
  prereqs[1] = {};
  prereqs[2] = {1};
  prereqs[3] = {1};
  prereqs[4] = {2};
  qg.Init(graph, prereqs);

  RecordingPublisher pub;
  Rig rig;
  simcore::PlayerInventoryStore store;
  auto storePtr = Rig::borrow(store);
  auto managerPtr = std::make_shared<simcore::QuestManager>(&qd, &qg, pub.callback());
  simcore::PlayerJoinedHandler joinHandler(storePtr, rig.router.client, managerPtr);

  pub.clear();
  joinHandler.handle(joinedPayload(kPlayer));

  // The QuestManager publishes an unlock burst for the roots it seeded. With an
  // empty QuestData there are no quests, so nothing is seeded; the point this
  // case pins is that the handler DID call the manager without touching the
  // inventory beyond initPlayer.
  CHECK(occupiedSlots(store.getSlots(kPlayer)) == 0,
        "the join seeds only the inventory entry, not inventory contents");
  CHECK_EQ(occupiedSlots(store.getSlots(kOtherPlayer)), 0,
           "and nothing at all for a player that has not joined");

  // A null manager must be tolerated — the guard is PlayerJoinedHandler.cpp:19.
  simcore::PlayerJoinedHandler noManager(storePtr, rig.router.client, nullptr);
  noManager.handle(joinedPayload(kOtherPlayer));
  CHECK(occupiedSlots(store.getSlots(kOtherPlayer)) == 0,
        "a join with no QuestManager still initialises the inventory");
}

// ---------------------------------------------------------------------------
// Both handlers are reachable through the ITopicHandler interface they are
// registered with
// ---------------------------------------------------------------------------
static void test_both_handlers_are_reachable_through_ITopicHandler() {
  Rig rig;
  simcore::PlayerInventoryStore store;
  auto storePtr = Rig::borrow(store);

  // This is exactly how SimCoreMessageHandler registers them, so a signature
  // change that breaks the dispatcher fails here too.
  std::unique_ptr<simcore::ITopicHandler> load =
      std::make_unique<simcore::InventoryLoadHandler>(storePtr, rig.router.client);
  std::unique_ptr<simcore::ITopicHandler> join =
      std::make_unique<simcore::PlayerJoinedHandler>(storePtr, rig.router.client, nullptr);

  CHECK(load != nullptr, "the load handler is an ITopicHandler");
  CHECK(join != nullptr, "the join handler is an ITopicHandler");

  Slots saved{};
  saved[0] = item(kItemA, 9);
  saved[39] = item(kItemB, 1, 2);
  load->handle(inventoryLoadPayload(kPlayer, saved));
  const Slots afterLoad = store.getSlots(kPlayer);
  CHECK_EQ(i_(afterLoad[0].count), 9, "the load applied through the interface");
  CHECK_EQ(i_(afterLoad[39].meta), 2, "including the last slot's meta");

  // A malformed buffer is what a dropped or truncated publish looks like. The
  // load must leave state intact.
  //
  // NOT ASSERTED HERE, DELIBERATELY: the empty-buffer case CRASHES. Both
  // handlers call flatbuffers::GetRoot on the raw pointer with no size check
  // and no Verifier, so data.data() is nullptr for an empty vector and the
  // first field read dereferences it:
  //     InventoryLoadHandler.cpp:10-11  GetRoot<InventoryUpdate> -> player_id()
  //     PlayerJoinedHandler.cpp:15-16   GetRoot<PlayerJoined>    -> player_id()
  // Confirmed by backtrace (frame #5 is the handler, #4 the generated
  // accessor, this=0x0 in every flatbuffers frame). A crash cannot be asserted
  // in-process without taking the whole suite down, so the call is simply not
  // made and the defect is filed as gp-qxbu-fix-1 with a regression test that
  // the fix should add. Everything else in these two 22/32-line handlers is
  // pinned below.
  //
  // The scale: "player.joined" and "player.inventory.load" are both subscribed
  // router topics (main.cpp:589, messageHandler.subscribeAll()), so a
  // zero-length or truncated publish from any peer takes down the whole SimCore
  // daemon, not just one player's join.
  CHECK(sameSlots(store.getSlots(kPlayer), afterLoad),
        "state is intact after the valid load");
}

static void test_handle_is_synchronous_on_the_calling_thread() {
  Rig rig;
  simcore::PlayerInventoryStore store;
  auto storePtr = Rig::borrow(store);
  simcore::InventoryLoadHandler loadHandler(storePtr, rig.router.client);

  Slots saved{};
  saved[0] = item(kItemA, 4);
  loadHandler.handle(inventoryLoadPayload(kPlayer, saved));
  // No polling, no sleep: the effect is already committed when handle returns.
  CHECK_EQ(i_(store.getSlots(kPlayer)[0].count), 4,
           "the load is applied inline, before handle() returns");
}

// ---------------------------------------------------------------------------
// The load republishes a snapshot the client can render
// ---------------------------------------------------------------------------
static void test_the_load_result_is_a_renderable_snapshot() {
  // InventoryLoadHandler.cpp:18-20 rebuilds the whole InventoryUpdate after
  // applying the load and publishes it. The republish is a no-op here (the
  // router is offline), so this case pins the BUFFER the handler builds: it is
  // the same buildUpdate call, observed through a store the test owns, to
  // prove the post-load snapshot carries 40 slots and the cursor rather than the
  // partial vector that arrived.
  Rig rig;
  simcore::PlayerInventoryStore store;
  auto storePtr = Rig::borrow(store);
  simcore::InventoryLoadHandler loadHandler(storePtr, rig.router.client);

  Slots saved{};
  saved[0] = item(kItemA, 8);
  saved[4] = item(kItemB, 2, 1);
  // MetaDB saved 5 slots, not 40.
  loadHandler.handle(inventoryLoadPayload(kPlayer, saved, 5));

  const Slots after = store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].count), 8, "the 5 saved slots landed");
  CHECK_EQ(i_(after[4].meta), 1, "with their meta");
  CHECK_EQ(occupiedSlots(after), 2, "only the saved slots are occupied");
  CHECK_EQ(i_(after[5].item_id), 0, "slot 5 was never in the save");

  // The snapshot the handler publishes is 40 wide — the client's grid size.
  flatbuffers::FlatBufferBuilder fb(512);
  auto off = store.buildUpdate(fb, kPlayer);
  fb.Finish(off);
  const auto* up = flatbuffers::GetRoot<Protocol::InventoryUpdate>(fb.GetBufferPointer());
  CHECK(up->slots() != nullptr, "the snapshot has a slot vector");
  if (up->slots()) {
    CHECK_EQ(i_(up->slots()->size()), kSlots,
             "the republished snapshot always carries all 40 slots, even when "
             "the load only carried 5");
  }
  CHECK_EQ(i_(up->player_id()), kPlayer, "for the player that loaded");
}

static void test_a_second_players_load_does_not_disturb_the_first() {
  Rig rig;
  simcore::PlayerInventoryStore store;
  auto storePtr = Rig::borrow(store);
  simcore::InventoryLoadHandler loadHandler(storePtr, rig.router.client);

  Slots a{};
  a[0] = item(kItemA, 10);
  Slots b{};
  b[0] = item(kItemB, 20);
  loadHandler.handle(inventoryLoadPayload(kPlayer, a));
  loadHandler.handle(inventoryLoadPayload(kOtherPlayer, b));

  CHECK_EQ(totalOf(store.getSlots(kPlayer), kItemA), 10, "player 1 kept its load");
  CHECK_EQ(totalOf(store.getSlots(kPlayer), kItemB), 0, "and did not gain player 2's items");
  CHECK_EQ(totalOf(store.getSlots(kOtherPlayer), kItemB), 20, "player 2 got its own load");
  CHECK_EQ(totalOf(store.getSlots(kOtherPlayer), kItemA), 0, "and nothing of player 1's");
}

static void test_a_reload_replaces_the_previous_save_rather_than_merging() {
  // OBSERVED: applyUpdate (PlayerInventoryStore.cpp:74-80) OVERWRITES the
  // leading slots. A shorter save therefore leaves a TAIL of the previous save
  // in place — the handler does not clear the inventory first. Pinned as
  // observed, with the reasoning, because it is the one way a restore can
  // produce a hybrid the player never had.
  Rig rig;
  simcore::PlayerInventoryStore store;
  auto storePtr = Rig::borrow(store);
  simcore::InventoryLoadHandler loadHandler(storePtr, rig.router.client);

  Slots first{};
  first[0] = item(kItemA, 1);
  first[1] = item(kItemB, 2);
  first[2] = item(kItemC, 3);
  loadHandler.handle(inventoryLoadPayload(kPlayer, first, 3));

  Slots second{};
  second[0] = item(kItemA, 99);
  loadHandler.handle(inventoryLoadPayload(kPlayer, second, 1));

  const Slots after = store.getSlots(kPlayer);
  CHECK_EQ(i_(after[0].count), 99, "the overlapping slot was overwritten");
  CHECK_EQ(i_(after[1].item_id), i_(kItemB),
           "OBSERVED: slot 1 kept item B from the PREVIOUS save");
  CHECK_EQ(i_(after[1].count), 2, "OBSERVED: with its previous count");
  CHECK_EQ(i_(after[2].item_id), i_(kItemC),
           "OBSERVED: slot 2 also survived from the previous save");
  CHECK(totalOf(after, kItemA) == 99, "the overwritten item shows the new count only");
  CHECK(occupiedSlots(after) == 3,
        "OBSERVED: the hybrid holds 3 slots, not the 1 the new save declares");
}

static void test_zero_player_id_is_handled_without_crashing() {
  // OBSERVED: neither handler rejects player_id 0. PlayerJoinedHandler warns
  // inside QuestManager and InventoryLoadHandler applies the update as-is, so
  // a zero-id publish creates a real (if meaningless) player entry. Asserted
  // as observed: the point is that the call is safe and deterministic.
  Rig rig;
  simcore::PlayerInventoryStore store;
  auto storePtr = Rig::borrow(store);
  simcore::InventoryLoadHandler loadHandler(storePtr, rig.router.client);
  simcore::PlayerJoinedHandler joinHandler(storePtr, rig.router.client, nullptr);

  Slots saved{};
  saved[0] = item(kItemA, 6);
  loadHandler.handle(inventoryLoadPayload(0, saved));
  CHECK_EQ(totalOf(store.getSlots(0), kItemA), 6,
           "OBSERVED: a player_id 0 load is applied, not rejected");
  joinHandler.handle(joinedPayload(0));
  // initPlayer uses try_emplace (PlayerInventoryStore.cpp:71), so it is a
  // no-op on an id that already has an entry — the join does not clear the
  // inventory the load had just written.
  CHECK_EQ(totalOf(store.getSlots(0), kItemA), 6,
           "OBSERVED: the join does not clear what the load had written");
  CHECK(occupiedSlots(store.getSlots(0)) == 1, "and the loaded item survives");
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("=== player_join_bootstrap test suite ===\n\n");

  TEST(the_bootstrap_topics_are_the_registered_ones);
  TEST(joining_with_an_existing_save_loads_that_inventory);
  TEST(joining_fresh_yields_an_empty_inventory_not_a_stale_one);
  TEST(a_fresh_join_does_not_inherit_another_players_inventory);
  TEST(the_join_handler_publishes_once_per_join);
  TEST(the_join_handler_seeds_the_quest_manager);
  TEST(both_handlers_are_reachable_through_ITopicHandler);
  TEST(handle_is_synchronous_on_the_calling_thread);
  TEST(the_load_result_is_a_renderable_snapshot);
  TEST(a_second_players_load_does_not_disturb_the_first);
  TEST(a_reload_replaces_the_previous_save_rather_than_merging);
  TEST(zero_player_id_is_handled_without_crashing);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
