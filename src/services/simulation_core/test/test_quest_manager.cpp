// QuestManager::completeQuest — manual (server-authoritative) quest completion.
// Uses the real data/quests/quests.csv + quest_graph.json (chain 1 → 2 → 3 …).
#include <libgtnh-net/test/test.h>

#include "Quest/QuestManager.h"
#include "quest_lib/QuestData.h"
#include "quest_lib/QuestGraph.h"
#include "quest_generated.h"
#include "Storage/PlayerInventoryStore.h"
#include <recipe_manager_lib/ItemRegistry.h>
#include <common/ItemId.h>
#include <flatbuffers/flatbuffers.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef PASS
#define PASS() do { ++g_passed; } while(0)
#endif

namespace {

struct PublishedMsg {
  std::string topic;
  std::vector<uint8_t> data;
};

// Records everything the QuestManager publishes so tests can assert on the
// exact events that drive MetaDB (reward grant) and the client.
struct RecordingPublisher {
  std::vector<PublishedMsg> msgs;
  std::function<void(const std::string&, const uint8_t*, size_t)> callback() {
    return [this](const std::string& topic, const uint8_t* data, size_t len) {
      msgs.push_back(PublishedMsg{topic, std::vector<uint8_t>(data, data + len)});
    };
  }
};

// One (quest_id, status, progress) entry for injecting quest progress.
struct ProgressEntry {
  uint32_t id;
  uint8_t status;
  uint8_t progress;
};

// Build a QuestProgressUpdate with the given entries — mirrors what MetaDB
// replies with on quest.get.
flatbuffers::Offset<Protocol::QuestProgressUpdate>
buildProgress(flatbuffers::FlatBufferBuilder& b, uint64_t playerId,
              const std::vector<ProgressEntry>& entries) {
  std::vector<flatbuffers::Offset<Protocol::QuestEntry>> offs;
  for (const auto& e : entries) {
    offs.push_back(Protocol::CreateQuestEntry(
        b, e.id, static_cast<Protocol::QuestStatus>(e.status), e.progress));
  }
  auto vec = b.CreateVector(offs);
  return Protocol::CreateQuestProgressUpdate(b, playerId, vec);
}

// Returns the status the QuestManager advertised for `questId` in the most
// recent quest.progress.updated message (its authoritative state broadcast).
int lastAdvertisedStatus(const RecordingPublisher& pub, uint32_t questId) {
  for (auto it = pub.msgs.rbegin(); it != pub.msgs.rend(); ++it) {
    if (it->topic != "quest.progress.updated") continue;
    flatbuffers::Verifier v(it->data.data(), it->data.size());
    if (!v.VerifyBuffer<Protocol::QuestProgressUpdate>(nullptr)) continue;
    auto* u = flatbuffers::GetRoot<Protocol::QuestProgressUpdate>(it->data.data());
    if (!u || !u->quests()) continue;
    for (size_t i = 0; i < u->quests()->size(); ++i) {
      auto* qe = u->quests()->Get(i);
      if (qe && qe->quest_id() == questId)
        return static_cast<int>(qe->status());
    }
  }
  return -1;
}

int countTopic(const RecordingPublisher& pub, const std::string& topic) {
  int n = 0;
  for (const auto& m : pub.msgs) if (m.topic == topic) ++n;
  return n;
}

// Shared fixture: loads real quest data and a QuestManager whose publishes
// are recorded, with player `player` seeded so quest 1 is COMPLETED and
// quest 2 is AVAILABLE (chain 1 → 2 → 3).
struct QuestFixture {
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr;

  explicit QuestFixture(uint64_t player)
      : mgr(&qd, &graph, pub.callback()) {
    qd.LoadCSV(DATA_DIR "/quests/quests.csv");
    qd.LoadGraph(DATA_DIR "/quests/quest_graph.json");
    std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
    for (const auto& q : qd.AllQuests()) prereqs[q.id] = q.prerequisites;
    graph.Init(qd.Graph(), prereqs);

    mgr.onPlayerJoined(player); // seeds all quests LOCKED
    flatbuffers::FlatBufferBuilder b(128);
    auto off = buildProgress(b, player,
        {{1, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100},
         {2, static_cast<uint8_t>(quest::QuestStatus::AVAILABLE), 0}});
    b.Finish(off);
    mgr.loadProgress(player, std::vector<uint8_t>(b.GetBufferPointer(),
                                                  b.GetBufferPointer() + b.GetSize()));
  }
};

// Loads real quest data, initializes the QuestGraph, and makes the item
// registry available so idToHierarchical() resolves packed → hierarchical
// (required for detection-path matching). Idempotent via ItemRegistry's
// loaded_ guard.
void buildManager(quest::QuestData& qd, quest::QuestGraph& graph) {
  qd.LoadCSV(DATA_DIR "/quests/quests.csv");
  qd.LoadGraph(DATA_DIR "/quests/quest_graph.json");
  std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
  for (const auto& q : qd.AllQuests()) prereqs[q.id] = q.prerequisites;
  graph.Init(qd.Graph(), prereqs);
  RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
}

// Seeds `entries` as the player's quest progress (via loadProgress).
void seedProgress(simcore::QuestManager& mgr, uint64_t player,
                  const std::vector<ProgressEntry>& entries) {
  mgr.onPlayerJoined(player); // seeds all quests LOCKED
  flatbuffers::FlatBufferBuilder b(128);
  auto off = buildProgress(b, player, entries);
  b.Finish(off);
  mgr.loadProgress(player, std::vector<uint8_t>(b.GetBufferPointer(),
                                                b.GetBufferPointer() + b.GetSize()));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 5.1 / 5.3: AVAILABLE quest with prereqs met → COMPLETED + quest.completed
// (reward event); re-completion does not double-grant.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_completeQuest_valid_and_idempotent() {
  const uint64_t player = 42;
  QuestFixture fx(player);

  // Quest 2 is AVAILABLE and its prereq (1) is COMPLETED → accepted.
  CHECK(fx.mgr.completeQuest(player, 2), "completeQuest(2) accepted");
  CHECK_EQ(countTopic(fx.pub, "quest.completed"), 1, "quest.completed published once");
  CHECK_EQ(lastAdvertisedStatus(fx.pub, 2),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "quest 2 advertised COMPLETED after manual completion");

  // Re-completion is rejected → no second quest.completed (no double grant).
  CHECK(!fx.mgr.completeQuest(player, 2), "re-completeQuest(2) rejected");
  CHECK_EQ(countTopic(fx.pub, "quest.completed"), 1,
           "re-completion must not re-publish quest.completed (no double grant)");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 5.2: LOCKED / already-COMPLETED / unknown quests are rejected with no
// state change and no reward event.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_completeQuest_rejects_invalid() {
  const uint64_t player = 7;
  QuestFixture fx(player);

  const int completedBefore = countTopic(fx.pub, "quest.completed");
  const int statusBroadcastsBefore = countTopic(fx.pub, "quest.progress.updated");

  // LOCKED quest (prereq 1 completed but never made AVAILABLE) → rejected.
  CHECK(!fx.mgr.completeQuest(player, 3), "LOCKED quest rejected");
  // Already-COMPLETED quest → rejected.
  CHECK(!fx.mgr.completeQuest(player, 1), "already-COMPLETED quest rejected");
  // Unknown quest id → rejected.
  CHECK(!fx.mgr.completeQuest(player, 9999), "unknown quest rejected");

  CHECK_EQ(countTopic(fx.pub, "quest.completed"), completedBefore,
           "no quest.completed on any rejected request (no reward)");
  CHECK_EQ(countTopic(fx.pub, "quest.progress.updated"), statusBroadcastsBefore,
           "no status broadcast on rejected requests (state unchanged)");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 4.1: EXCHANGE quests (repeatable market) must never complete via
// completeQuest — the flow is MetaDB-owned (quest.exchange.request), and a
// manual/accidental completion would break the repeatable market contract.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_completeQuest_rejects_exchange() {
  const uint64_t player = 71;
  QuestFixture fx(player);

  // Quest 4 is EXCHANGE; make it AVAILABLE so a naive guard placement
  // (status check first) would accept it.
  flatbuffers::FlatBufferBuilder b(128);
  auto off = buildProgress(b, player,
      {{4, static_cast<uint8_t>(quest::QuestStatus::AVAILABLE), 0}});
  b.Finish(off);
  fx.mgr.loadProgress(player, std::vector<uint8_t>(b.GetBufferPointer(),
                                                   b.GetBufferPointer() + b.GetSize()));

  CHECK(!fx.mgr.completeQuest(player, 4), "EXCHANGE quest rejected by completeQuest");
  CHECK_EQ(countTopic(fx.pub, "quest.completed"), 0,
           "no quest.completed for EXCHANGE quest (no reward)");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.5: BuildQuestEraMap() excludes EXCHANGE quests — an exchange quest can
// never reach COMPLETED, so counting it toward era completion would deadlock
// the era forever. Quest 4 is the exchange quest in VAGRANT.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestData_eraMap_excludes_exchange() {
  quest::QuestData qd;
  CHECK(qd.LoadCSV(DATA_DIR "/quests/quests.csv"), "LoadCSV ok");
  auto eraMap = qd.BuildQuestEraMap();
  CHECK(eraMap.find(4) == eraMap.end(), "exchange quest 4 excluded from era map");
  CHECK(eraMap.find(1) != eraMap.end(), "non-exchange quest 1 still in era map");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Unlock propagation: completing quest 2 unlocks quest 3 (prereq met), which
// is advertised AVAILABLE and published on quest.unlocked.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_completeQuest_unlocks_dependents() {
  const uint64_t player = 99;
  QuestFixture fx(player);

  CHECK(fx.mgr.completeQuest(player, 2), "completeQuest(2) accepted");
  CHECK_EQ(countTopic(fx.pub, "quest.unlocked"), 2,
           "quest.unlocked published (auto + completion cascade)");
  CHECK_EQ(lastAdvertisedStatus(fx.pub, 3),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "dependent quest 3 advertised AVAILABLE after prereq 2 completed");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Era transition: VAGRANT quests are 1,2,3,4,5,6,37,38,39. Seed all but quest
// 3 COMPLETED; completing quest 3 closes the era → quest.era.transition is
// published exactly once with completed_era=VAGRANT(0), next_era=APPRENTICE(1).
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_eraTransition_published_once() {
  const uint64_t player = 55;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  qd.LoadCSV(DATA_DIR "/quests/quests.csv");
  qd.LoadGraph(DATA_DIR "/quests/quest_graph.json");
  std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
  for (const auto& q : qd.AllQuests()) prereqs[q.id] = q.prerequisites;
  graph.Init(qd.Graph(), prereqs);

  mgr.onPlayerJoined(player);
  flatbuffers::FlatBufferBuilder b(256);
  auto off = buildProgress(b, player,
      {{1, 3, 100}, {2, 3, 100}, {4, 3, 100}, {5, 3, 100},
       {6, 3, 100}, {37, 3, 100}, {38, 3, 100}, {39, 3, 100},
       {3, 1, 0}});
  b.Finish(off);
  mgr.loadProgress(player, std::vector<uint8_t>(b.GetBufferPointer(),
                                                b.GetBufferPointer() + b.GetSize()));

  CHECK_EQ(countTopic(pub, "quest.era.transition"), 0,
           "no era transition before last VAGRANT quest completes");
  CHECK(mgr.completeQuest(player, 3), "completeQuest(3) accepted");
  CHECK_EQ(countTopic(pub, "quest.era.transition"), 1,
           "era transition published exactly once");

  const PublishedMsg* eraMsg = nullptr;
  for (auto it = pub.msgs.rbegin(); it != pub.msgs.rend(); ++it) {
    if (it->topic == "quest.era.transition") { eraMsg = &*it; break; }
  }
  CHECK(eraMsg != nullptr, "era transition message present");
  if (eraMsg) {
    flatbuffers::Verifier v(eraMsg->data.data(), eraMsg->data.size());
    if (v.VerifyBuffer<Protocol::EraTransitionNotification>(nullptr)) {
      auto* era = flatbuffers::GetRoot<Protocol::EraTransitionNotification>(eraMsg->data.data());
      CHECK_EQ(era->completed_era(), static_cast<uint8_t>(quest::Era::VAGRANT),
               "completed era is VAGRANT");
      CHECK_EQ(era->next_era(), static_cast<uint8_t>(quest::Era::APPRENTICE),
               "next era is APPRENTICE");
    }
  }
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// One-step detection (task 1.1/1.2): a LOCKED quest whose prerequisites are
// met completes directly via the detection path. Regression for the
// dead-detection bug — onPlayerJoined seeds every quest LOCKED, so the old
// "make AVAILABLE" branch was unreachable and craft/block quests never
// completed for joined players. Also verifies the unlock cascade publishes
// quest.unlocked (task 3.1).
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_detection_one_step_craft() {
  const uint64_t player = 21;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Quest 1 COMPLETED; quest 2 (craft iron_ingot, prereq 1) stays LOCKED.
  seedProgress(mgr, player,
      {{1, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  mgr.checkCraftCompletion(player, ItemId::pack("0:110:1"), 1);
  CHECK_EQ(lastAdvertisedStatus(pub, 2),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "LOCKED craft quest completes in one step via detection");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "quest.completed published once");

  // Completing quest 2 cascades unlocks: quest 3 (prereq 2) → AVAILABLE.
  CHECK_EQ(countTopic(pub, "quest.unlocked"), 2,
           "quest.unlocked published (auto + detection cascade)");
  CHECK_EQ(lastAdvertisedStatus(pub, 3),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "dependent quest 3 advertised AVAILABLE after detection");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// TOOL_CHARGED detection (task 2.1): checkToolCharged completes quest 40
// (ULV drill, prereq 18) when the packed drill id matches detectTarget.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_detection_tool_charged() {
  const uint64_t player = 31;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Quest 18 (ULV Drilling, prereq of 40) COMPLETED; quest 40 stays LOCKED.
  seedProgress(mgr, player,
      {{18, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  mgr.checkToolCharged(player, ItemId::pack("1111:00:0"));
  CHECK_EQ(lastAdvertisedStatus(pub, 40),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "TOOL_CHARGED quest 40 completes via checkToolCharged");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "quest.completed published once");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// SIDE_CONFIGURED detection (task 2.2): checkSideConfigured completes quest
// 41 (heat furnace, prereq 22) when the packed machine id matches. A
// machine_id of 0 (multiblock hatch path) is not a side-config target.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_detection_side_configured() {
  const uint64_t player = 41;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Quest 22 (Wrench Mastery, prereq of 41) COMPLETED; quest 41 stays LOCKED.
  seedProgress(mgr, player,
      {{22, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  // Hatch wrenching (machine_id == 0) must not complete side-config quests.
  mgr.checkSideConfigured(player, 0);
  CHECK_EQ(countTopic(pub, "quest.completed"), 0,
           "machine_id 0 (hatch) does not complete side-config quest");

  mgr.checkSideConfigured(player, ItemId::pack("1110:00:0"));
  CHECK_EQ(lastAdvertisedStatus(pub, 41),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "SIDE_CONFIGURED quest 41 completes via checkSideConfigured");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "quest.completed published once");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// INVENTORY detection (quest book open): checkInventory completes quest 42
// (copper ore 10:3, prereq 7, target_count 8) only when the player holds the
// target quantity. Below-target and unmet-prereq cases are no-ops. Multiple
// slots holding the item aggregate.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_detection_inventory() {
  const uint64_t player = 51;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Quest 7 (Bronze Age, prereq of 42) COMPLETED; quest 42 stays LOCKED.
  seedProgress(mgr, player,
      {{7, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  // Below target: 5 copper ore < 8 required → no completion.
  std::vector<simcore::PersistSlot> below;
  below.push_back({ItemId::pack("10:3"), 5, 0});
  mgr.checkInventory(player, below);
  CHECK_EQ(countTopic(pub, "quest.completed"), 0,
           "below-target inventory does not complete quest 42");

  // At target: slots aggregate (3 + 5 = 8) → quest 42 completes.
  std::vector<simcore::PersistSlot> met;
  met.push_back({ItemId::pack("10:3"), 3, 0});
  met.push_back({ItemId::pack("10:3"), 5, 0});
  mgr.checkInventory(player, met);
  CHECK_EQ(lastAdvertisedStatus(pub, 42),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "INVENTORY quest 42 completes when target quantity held");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "quest.completed published once");

  // Prereq gate: quest 43 (iron ore, prereq 18 not completed) must not
  // complete even with enough iron ore held.
  std::vector<simcore::PersistSlot> iron;
  iron.push_back({ItemId::pack("10:0"), 32, 0});
  mgr.checkInventory(player, iron);
  CHECK_EQ(lastAdvertisedStatus(pub, 43), -1,
           "INVENTORY quest 43 not completed when prereq unmet");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "no extra quest.completed for unmet-prereq inventory quest");
  PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

void test_quest_manager() {
  TEST(QuestManager_completeQuest_valid_and_idempotent);
  TEST(QuestManager_completeQuest_rejects_invalid);
  TEST(QuestManager_completeQuest_rejects_exchange);
  TEST(QuestData_eraMap_excludes_exchange);
  TEST(QuestManager_completeQuest_unlocks_dependents);
  TEST(QuestManager_eraTransition_published_once);
  TEST(QuestManager_detection_one_step_craft);
  TEST(QuestManager_detection_tool_charged);
  TEST(QuestManager_detection_side_configured);
  TEST(QuestManager_detection_inventory);
}
