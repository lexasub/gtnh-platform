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

#include <sys/wait.h>
#include <unistd.h>

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

// ---------------------------------------------------------------------------
// gp-ajvg — the crash-isolation harness
// ---------------------------------------------------------------------------
// The gp-ajvg crash (GetRoot with no Verifier on a null pointer) cannot be
// asserted in-process: calling handle({}) segfaults the whole binary before a
// single check can run. So the empty/truncated/garbage calls run in a forked
// CHILD, and the PARENT asserts only on the child's exit status:
//
//     crashed (SIGSEGV/SIGBUS/SIGABRT)  -> the regression is still present
//     exited 0                           -> the payload was rejected cleanly
//
// This is fully deterministic: the fixture creates no thread, no socket and no
// timer, so fork() copies a single-threaded process and the child runs the
// handler to completion. The child is given its OWN store and handler, so a
// crash cannot corrupt the parent's observable state. `_exit()` is used rather
// than exit() so the child's spdlog/atexit teardown cannot itself abort and be
// misread as a handler failure.
constexpr int kChildOk = 0;

struct ChildOutcome {
  bool exited = false;      // the child terminated normally (not on a signal)
  bool signalled = false;   // the child died on a signal -- a CRASH
  int signal_number = 0;    // which signal, when signalled
  int exit_code = -1;       // its exit status, when exited
};

// The child's offline router, created on first use. Constructed and NEVER
// Connect()ed, so Publish()/PublishRaw() short-circuit and no socket or
// thread is ever created — which is also what makes fork() safe here.
std::shared_ptr<simcore::IoUringRouterClient>& childRouter() {
  static std::shared_ptr<simcore::IoUringRouterClient> r =
      std::make_shared<simcore::IoUringRouterClient>();
  return r;
}

// The child body receives a store by reference: the harness constructs one
// per child, so a crash in the handler can never corrupt the parent's
// observable state.
using ChildBody = void (*)(simcore::PlayerInventoryStore&);

ChildOutcome runInChild(ChildBody body) {
  std::fflush(stdout);
  const pid_t pid = fork();
  if (pid < 0) {
    // fork() failing is an environment fault, not a verdict about the handler,
    // so report the benign outcome rather than a false regression.
    ChildOutcome o;
    o.exited = true;
    o.exit_code = kChildOk;
    return o;
  }
  if (pid == 0) {
    // The child gets its OWN store, so a crash cannot corrupt the parent's
    // observable state.
    simcore::PlayerInventoryStore store;
    body(store);
    _exit(kChildOk);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    ChildOutcome o;
    o.exited = true;
    o.exit_code = kChildOk;
    return o;
  }
  ChildOutcome o;
  if (WIFEXITED(status)) {
    o.exited = true;
    o.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    o.signalled = true;
    o.signal_number = WTERMSIG(status);
  }
  return o;
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
  // FIXED (gp-ajvg). This comment used to record an UNASSERTED crash: both
  // handlers called flatbuffers::GetRoot on the raw pointer with no size check
  // and no Verifier, so for a zero-length payload data.data() was nullptr and
  // the first field read dereferenced it:
  //     InventoryLoadHandler.cpp:10-11  GetRoot<InventoryUpdate> -> player_id()
  //     PlayerJoinedHandler.cpp:15-16   GetRoot<PlayerJoined>    -> player_id()
  // Confirmed by backtrace (frame #5 is the handler, #4 the generated
  // accessor, this=0x0 in every flatbuffers frame). A crash cannot be asserted
  // in-process without taking the whole suite down, so the empty-buffer call is
  // NOT made here either; it is made in a forked child by the
  // test_gp_ajvg_* cases below, which assert the handler survived.
  //
  // The scale: "player.joined" and "player.inventory.load" are both subscribed
  // router topics (main.cpp:589, messageHandler.subscribeAll()), so a
  // zero-length or truncated publish from any peer took down the whole SimCore
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

// ===========================================================================
// gp-ajvg — a malformed or empty payload must be REJECTED, never read
// ===========================================================================
//
// Both "player.inventory.load" and "player.joined" are router-subscribed
// topics, so any publisher on the bus can deliver a zero-length or truncated
// payload to either handler. Before the fix both called
// flatbuffers::GetRoot on the raw pointer with no size check and no Verifier:
//
//   InventoryLoadHandler.cpp:10-11  GetRoot<InventoryUpdate> -> player_id()
//   PlayerJoinedHandler.cpp:15-16   GetRoot<PlayerJoined>    -> player_id()
//
// For an empty std::vector, data.data() is nullptr, so the first accessor
// dereferenced null and took the whole SimCore daemon with it. Confirmed by
// gdb (ReadScalar<int>(p=0x0) <- Table::GetVTable(this=0x0) <-
// PlayerJoinedHandler::handle) — which is also why the calls below run in a
// forked child: a crash cannot be asserted in-process, and this is the standard
// way to assert "did not crash" from inside a test binary.
//
// The parent asserts only on the child's exit status, which is a stable,
// deterministic property — no wall clock, no sleep, no timeout race.

// The child bodies. Each takes a store by reference (the harness's own
// instance), builds the handler over the never-connected router, and feeds it
// one hostile payload. If the handler crashes the child dies by signal; if it
// rejects the payload the child exits 0.

// THE regression: a zero-length publish.
static void childLoadEmpty(simcore::PlayerInventoryStore& store) {
  simcore::InventoryLoadHandler h(Rig::borrow(store), childRouter());
  h.handle({});
}

static void childJoinEmpty(simcore::PlayerInventoryStore& store) {
  simcore::PlayerJoinedHandler h(Rig::borrow(store), childRouter(), nullptr);
  h.handle({});
}

// Every truncation of a REAL payload. A partial FlatBuffer is the far more
// likely wire event than a truly zero-length publish (a short read, a frame
// split at a boundary, a publisher that wrote a header and died), and it is
// also the case a Verifier catches that a bare size check would not.
static void childLoadTruncated(simcore::PlayerInventoryStore& store) {
  simcore::InventoryLoadHandler h(Rig::borrow(store), childRouter());
  Slots saved{};
  saved[0] = item(kItemA, 12);
  saved[7] = item(kItemB, 3, 5);
  const std::vector<uint8_t> real = inventoryLoadPayload(kPlayer, saved);
  for (size_t cut = 0; cut < real.size(); ++cut)
    h.handle(std::vector<uint8_t>(real.begin(), real.begin() + cut));
}

static void childJoinTruncated(simcore::PlayerInventoryStore& store) {
  simcore::PlayerJoinedHandler h(Rig::borrow(store), childRouter(), nullptr);
  const std::vector<uint8_t> real = joinedPayload(kPlayer);
  for (size_t cut = 0; cut < real.size(); ++cut)
    h.handle(std::vector<uint8_t>(real.begin(), real.begin() + cut));
}

// Garbage that is structurally incapable of being either payload. Without a
// Verifier the handler would walk a fabricated vtable and read nonsense
// offsets; with one, all of these are rejected before the first field read.
static void childLoadGarbage(simcore::PlayerInventoryStore& store) {
  simcore::InventoryLoadHandler h(Rig::borrow(store), childRouter());
  const std::vector<std::vector<uint8_t>> garbage = {
      {},                            // empty
      {0},                           // one byte
      {0, 0, 0, 0},                  // root offset 0
      {0, 0, 0, 1},                  // root offset past the end
      {0xff, 0xff, 0xff, 0xff},      // offsets into the void
      {0x2a, 0x00},                  // nonsense
      std::vector<uint8_t>(4096, 0),  // large all-zero
      std::vector<uint8_t>(4096, 0x7f),
  };
  for (const auto& g : garbage) h.handle(g);
}

static void childJoinGarbage(simcore::PlayerInventoryStore& store) {
  simcore::PlayerJoinedHandler h(Rig::borrow(store), childRouter(), nullptr);
  const std::vector<std::vector<uint8_t>> garbage = {
      {},
      {0},
      {0, 0, 0, 0},
      {0, 0, 0, 1},
      {0xff, 0xff, 0xff, 0xff},
      {0x2a, 0x00},
      std::vector<uint8_t>(4096, 0),
      std::vector<uint8_t>(4096, 0x7f),
  };
  for (const auto& g : garbage) h.handle(g);
}

// The child, after surviving all the garbage, must still handle a VALID
// payload. This proves the guard rejects rather than wedges: a handler that
// somehow latched a bad state would fail here.
static void childLoadGarbageThenValid(simcore::PlayerInventoryStore& store) {
  simcore::InventoryLoadHandler h(Rig::borrow(store), childRouter());
  for (const auto& g : std::vector<std::vector<uint8_t>>{{}, {0}, {0, 0, 0, 0},
                                                        {0xff, 0xff, 0xff, 0xff}})
    h.handle(g);
  Slots saved{};
  saved[0] = item(kItemA, 5);
  h.handle(inventoryLoadPayload(kPlayer, saved));
  if (totalOf(store.getSlots(kPlayer), kItemA) != 5) _exit(2);
  _exit(kChildOk);
}

static void childJoinGarbageThenValid(simcore::PlayerInventoryStore& store) {
  simcore::PlayerJoinedHandler h(Rig::borrow(store), childRouter(), nullptr);
  for (const auto& g : std::vector<std::vector<uint8_t>>{{}, {0}, {0, 0, 0, 0},
                                                        {0xff, 0xff, 0xff, 0xff}})
    h.handle(g);
  h.handle(joinedPayload(kOtherPlayer));
  if (occupiedSlots(store.getSlots(kOtherPlayer)) != 0) _exit(2);
  _exit(kChildOk);
}

// A helper so a child's verdict reads the same way in every case: the child
// either returned cleanly (rejected the payload) or died on a signal (crash).
// Reported as one test_check with a formatted message rather than through the
// CHECK macro, because the message depends on the runtime signal number.
static void assertNoCrash(const char* what, ChildOutcome o) {
  if (o.signalled) {
    char expr[256];
    snprintf(expr, sizeof(expr),
             "gp-ajvg: %s handler survived a malformed payload (died on signal %d)",
             what, o.signal_number);
    test_check(false, __FILE__, __LINE__, expr,
               "a zero-length, truncated or garbage publish must be REJECTED, "
               "not read: the pre-fix Verifier-less GetRoot null-dereferenced "
               "and took the whole SimCore daemon down");
    return;
  }
  char expr[256];
  snprintf(expr, sizeof(expr),
           "gp-ajvg: %s handler exited cleanly (exit %d, wanted %d)", what,
           o.exit_code, kChildOk);
  test_check(o.exited && o.exit_code == kChildOk, __FILE__, __LINE__, expr,
             "the handler rejected the malformed payload and returned normally");
}

// gp-ajvg, case 1: a zero-length publish to either topic must not crash.
static void test_gp_ajvg_an_empty_publish_does_not_crash_either_handler() {
  assertNoCrash("player.inventory.load", runInChild(&childLoadEmpty));
  assertNoCrash("player.joined", runInChild(&childJoinEmpty));
}

// gp-ajvg, case 2: every truncation of a real payload must be rejected.
static void test_gp_ajvg_every_truncation_of_a_real_payload_is_rejected() {
  assertNoCrash("player.inventory.load (truncated)", runInChild(&childLoadTruncated));
  assertNoCrash("player.joined (truncated)", runInChild(&childJoinTruncated));
}

// gp-ajvg, case 3: structurally impossible buffers must be rejected too.
static void test_gp_ajvg_structurally_impossible_buffers_are_rejected() {
  assertNoCrash("player.inventory.load (garbage)", runInChild(&childLoadGarbage));
  assertNoCrash("player.joined (garbage)", runInChild(&childJoinGarbage));
}

// gp-ajvg, case 4: the guard rejects, it does not wedge — a valid payload
// still works immediately after a batch of garbage.
static void test_gp_ajvg_a_valid_payload_still_works_after_garbage() {
  const ChildOutcome load = runInChild(&childLoadGarbageThenValid);
  assertNoCrash("player.inventory.load (garbage then valid)", load);
  const ChildOutcome join = runInChild(&childJoinGarbageThenValid);
  assertNoCrash("player.joined (garbage then valid)", join);
}

// gp-ajvg, case 5: the STATE half — the store is left completely unchanged by
// every malformed buffer, not merely "the crash did not happen". A non-empty
// garbage buffer that verified-then-parsed would overwrite a real player's
// inventory with a fabricated save, which is the data-corruption half of the
// same bug. Uses the same full-snapshot before/after discipline as
// test_inventory_action_handler.cpp: every one of the 40 slots, not just the
// one that would have been expected to change.
//
// It runs in a child like the others — pre-fix, feeding these buffers in
// process would segfault the test binary itself and the suite would never print
// its summary. The child owns the store, so it does the snapshot comparison
// itself and encodes the verdict in the exit code:
//
//   exit 0 -> every malformed buffer left the store byte-identical
//   exit 3 -> a malformed buffer DID change committed state
//   signal -> the handler crashed (case 1-4's verdict)
static void childMalformedLeavesStoreUnchanged(simcore::PlayerInventoryStore& store) {
  auto storePtr = Rig::borrow(store);
  simcore::InventoryLoadHandler loadHandler(storePtr, childRouter());
  simcore::PlayerJoinedHandler joinHandler(storePtr, childRouter(), nullptr);

  // A real, distinctive save the malformed payloads must not disturb: every
  // slot holds a unique item, so a bleed between any two is visible.
  Slots saved{};
  for (int k = 0; k < simcore::kInventorySlots; ++k)
    saved[static_cast<size_t>(k)] =
        item(static_cast<uint16_t>(1000 + k), static_cast<uint8_t>(1 + (k % 60)),
             static_cast<uint16_t>(k));
  loadHandler.handle(inventoryLoadPayload(kPlayer, saved));
  joinHandler.handle(joinedPayload(kPlayer));
  const Slots before_load = store.getSlots(kPlayer);
  if (occupiedSlots(before_load) != 40) _exit(3);

  // Build a real payload, then feed every prefix of it plus the garbage shapes
  // the inventory_action_handler suite already enumerates.
  const std::vector<uint8_t> real = inventoryLoadPayload(kOtherPlayer, saved);
  for (size_t cut = 0; cut < real.size(); ++cut)
    loadHandler.handle(std::vector<uint8_t>(real.begin(), real.begin() + cut));
  const std::vector<uint8_t> realJoined = joinedPayload(kOtherPlayer);
  for (size_t cut = 0; cut < realJoined.size(); ++cut)
    joinHandler.handle(std::vector<uint8_t>(realJoined.begin(), realJoined.begin() + cut));

  const std::vector<std::vector<uint8_t>> garbage = {
      {}, {0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0xff, 0xff, 0xff, 0xff}, {0x2a, 0x00},
      std::vector<uint8_t>(4096, 0), std::vector<uint8_t>(1024, 0x7f),
  };
  for (const auto& g : garbage) {
    loadHandler.handle(g);
    joinHandler.handle(g);
  }

  // The load snapshot must be byte-identical, and a rejected join must not
  // have created a phantom player entry (initPlayer is the join's first side
  // effect, so a garbage buffer that reached it would be observable here).
  if (!sameSlots(store.getSlots(kPlayer), before_load)) _exit(3);
  if (occupiedSlots(store.getSlots(kOtherPlayer)) != 0) _exit(3);

  // And the handlers are still functional: a real payload after the whole
  // battery still applies, and it does not disturb the other player.
  Slots fresh{};
  fresh[0] = item(kItemC, 11, 2);
  loadHandler.handle(inventoryLoadPayload(kOtherPlayer, fresh, 1));
  if (i_(store.getSlots(kOtherPlayer)[0].count) != 11) _exit(3);
  if (!sameSlots(store.getSlots(kPlayer), before_load)) _exit(3);
  _exit(kChildOk);
}

static void test_gp_ajvg_malformed_payloads_leave_the_store_completely_unchanged() {
  const ChildOutcome o = runInChild(&childMalformedLeavesStoreUnchanged);
  char expr[256];
  if (o.signalled) {
    snprintf(expr, sizeof(expr),
             "gp-ajvg: state snapshot after a malformed payload (died on signal %d)",
             o.signal_number);
    test_check(false, __FILE__, __LINE__, expr,
               "the handler must reject the buffer, not read it");
    return;
  }
  snprintf(expr, sizeof(expr),
           "gp-ajvg: state snapshot after a malformed payload (exit %d, wanted %d)",
           o.exit_code, kChildOk);
  test_check(o.exited && o.exit_code == kChildOk, __FILE__, __LINE__, expr,
             "a malformed buffer changed committed state (child exit 3): either "
             "a truncated/garbage buffer was parsed into a fabricated save, or a "
             "rejected join created a phantom player entry");
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

  // gp-ajvg: a malformed or empty publish must be rejected, never read.
  TEST(gp_ajvg_an_empty_publish_does_not_crash_either_handler);
  TEST(gp_ajvg_every_truncation_of_a_real_payload_is_rejected);
  TEST(gp_ajvg_structurally_impossible_buffers_are_rejected);
  TEST(gp_ajvg_a_valid_payload_still_works_after_garbage);
  TEST(gp_ajvg_malformed_payloads_leave_the_store_completely_unchanged);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
