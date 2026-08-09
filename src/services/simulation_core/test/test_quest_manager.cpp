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
// 3.5: BuildQuestEraMap() must cover every quest — the current data has no
// EXCHANGE quests, so nothing may be excluded from era completion counting.
// Quest 4 (Place Workbench, BLOCK_PLACED) is a normal progress quest.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestData_eraMap_covers_all_quests() {
  quest::QuestData qd;
  CHECK(qd.LoadCSV(DATA_DIR "/quests/quests.csv"), "LoadCSV ok");
  auto eraMap = qd.BuildQuestEraMap();
  CHECK(eraMap.size() == 163, "era map covers all 163 quests");
  CHECK(eraMap.find(4) != eraMap.end(), "block_placed quest 4 in era map");
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
// Era transition: complete every VAGRANT quest except quest 3; completing
// quest 3 closes the era → quest.era.transition is published exactly once
// with completed_era=VAGRANT(0), next_era=APPRENTICE(1). The seed set is
// derived from GetEraQuests(VAGRANT) so the test tracks the quest data.
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

  std::vector<ProgressEntry> seed;
  for (const auto* qdef : qd.GetEraQuests(quest::Era::VAGRANT)) {
    if (qdef->id == 3) {
      seed.push_back({3, static_cast<uint8_t>(quest::QuestStatus::AVAILABLE), 0});
    } else {
      seed.push_back({qdef->id, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100});
    }
  }

  mgr.onPlayerJoined(player);
  flatbuffers::FlatBufferBuilder b(512);
  auto off = buildProgress(b, player, seed);
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

  // Quest 1 COMPLETED; quest 2 (craft oak_planks, prereq 1) stays LOCKED.
  seedProgress(mgr, player,
      {{1, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  mgr.checkCraftCompletion(player, ItemId::pack("0:10:00:0"), 1);
  CHECK_EQ(lastAdvertisedStatus(pub, 2),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "LOCKED craft quest completes in one step via detection");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "quest.completed published once");

  // Completing quest 2 cascades unlocks: quest 3 (prereq 2) → AVAILABLE.
  CHECK_EQ(countTopic(pub, "quest.unlocked"), 3,
           "quest.unlocked published (auto + seed reconciliation + cascade)");
  CHECK_EQ(lastAdvertisedStatus(pub, 3),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "dependent quest 3 advertised AVAILABLE after detection");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// BLOCK_PLACED detection (task 2.1): checkBlockAction completes quest 14
// (Place Heat Furnace, prereq 11) when the placed block matches detectTarget.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_detection_block_placed() {
  const uint64_t player = 31;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Quest 11 (Heat Furnace, prereq of 14) COMPLETED; quest 14 stays LOCKED.
  seedProgress(mgr, player,
      {{11, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  mgr.checkBlockAction(player, 10, 20, 30, ItemId::pack("1110:00:0"));
  CHECK_EQ(lastAdvertisedStatus(pub, 14),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "BLOCK_PLACED quest 14 completes via checkBlockAction");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "quest.completed published once");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// SIDE_CONFIGURED detection (task 2.2): quest 71 (Side Configuration, prereqs
// 40+55) targets "1:0:0:0:9", which is absent from items.csv — so no packed
// machine id can satisfy it. Test the reachable invariants: hatch wrenching
// (machine_id == 0) and unrelated machines never complete a side-config quest.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_detection_side_configured() {
  const uint64_t player = 41;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Quest 40 (Wrench Mastery) and 55 (Electric Furnace LV), prereqs of 71,
  // COMPLETED; quest 71 stays LOCKED.
  seedProgress(mgr, player,
      {{40, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100},
       {55, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  // Hatch wrenching (machine_id == 0) must not complete side-config quests.
  mgr.checkSideConfigured(player, 0);
  CHECK_EQ(countTopic(pub, "quest.completed"), 0,
           "machine_id 0 (hatch) does not complete side-config quest");

  // Unrelated machine (heat furnace) does not match 71's detect target.
  mgr.checkSideConfigured(player, ItemId::pack("1110:00:0"));
  CHECK_EQ(countTopic(pub, "quest.completed"), 0,
           "unrelated machine does not complete side-config quest");
  CHECK_EQ(lastAdvertisedStatus(pub, 71),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "side-config quest 71 stays AVAILABLE (target not in items.csv)");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// INVENTORY detection (quest book open): checkInventory completes quest 23
// (Copper Prospecting, copper ore 10:3, prereq 14) when the target item is
// held — the CSV has no target_count column, so 1 suffices. Below-target and
// unmet-prereq cases are no-ops. Multiple slots holding the item aggregate.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_detection_inventory() {
  const uint64_t player = 51;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Quest 14 (Place Heat Furnace, prereq of 23) COMPLETED; quest 23 LOCKED.
  seedProgress(mgr, player,
      {{14, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});

  // Below target: no copper ore held → no completion.
  std::vector<simcore::PersistSlot> below;
  mgr.checkInventory(player, below);
  CHECK_EQ(countTopic(pub, "quest.completed"), 0,
           "empty inventory does not complete quest 23");

  // At target: slots aggregate (3 + 5 = 8 copper ore) → quest 23 completes.
  std::vector<simcore::PersistSlot> met;
  met.push_back({ItemId::pack("10:3"), 3, 0});
  met.push_back({ItemId::pack("10:3"), 5, 0});
  mgr.checkInventory(player, met);
  CHECK_EQ(lastAdvertisedStatus(pub, 23),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "INVENTORY quest 23 completes when copper ore held");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "quest.completed published once");

  // Prereq gate: quest 9 (Iron Prospecting, prereq 8 not completed) must not
  // complete even with enough iron ore held. After a loaded snapshot the
  // client knows its full state, so 9 is advertised LOCKED (not absent).
  std::vector<simcore::PersistSlot> iron;
  iron.push_back({ItemId::pack("10:0"), 32, 0});
  mgr.checkInventory(player, iron);
  CHECK_EQ(lastAdvertisedStatus(pub, 9),
           static_cast<int>(quest::QuestStatus::LOCKED),
           "INVENTORY quest 9 stays LOCKED when prereq unmet");
  CHECK_EQ(countTopic(pub, "quest.completed"), 1,
           "no extra quest.completed for unmet-prereq inventory quest");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Rejoin (reconnect): onPlayerJoined must not wipe in-memory progress. A
// player who completed quest 2 keeps it COMPLETED across a rejoin, and no
// re-unlock/re-completion events are published.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_rejoin_preserves_progress() {
  const uint64_t player = 61;
  QuestFixture fx(player);

  CHECK(fx.mgr.completeQuest(player, 2), "quest 2 completed before rejoin");
  CHECK_EQ(lastAdvertisedStatus(fx.pub, 2),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "quest 2 COMPLETED before rejoin");

  const int unlockedBefore = countTopic(fx.pub, "quest.unlocked");
  const int completedBefore = countTopic(fx.pub, "quest.completed");
  fx.mgr.onPlayerJoined(player);
  CHECK_EQ(countTopic(fx.pub, "quest.unlocked"), unlockedBefore,
           "rejoin publishes no new unlocks");
  CHECK_EQ(countTopic(fx.pub, "quest.completed"), completedBefore,
           "rejoin does not re-complete quests");
  CHECK_EQ(lastAdvertisedStatus(fx.pub, 2),
           static_cast<int>(quest::QuestStatus::COMPLETED),
           "quest 2 stays COMPLETED after rejoin");
  PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Reconciliation (regression): MetaDB may report quests COMPLETED outside the
// normal completion flow (forced/DB edit). loadProgress must unlock any LOCKED
// quest whose prerequisites are now satisfied — publishing AVAILABLE status +
// quest.unlocked — otherwise dependents stay locked forever (the quest-11-
//locked-while-quest-7-completed bug). Chain 1 → 2 → 3: loading {1: COMPLETED}
// unlocks 2; loading {2: COMPLETED} then unlocks 3.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_loadProgress_reconciles_unlocks() {
  const uint64_t player = 77;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  // Seed all quests (root 1 AVAILABLE, rest LOCKED), then report quest 1
  // COMPLETED straight from MetaDB — no detection, no completeQuest.
  mgr.onPlayerJoined(player);
  flatbuffers::FlatBufferBuilder b(128);
  auto off = buildProgress(b, player,
      {{1, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});
  b.Finish(off);
  mgr.loadProgress(player, std::vector<uint8_t>(b.GetBufferPointer(),
                                                b.GetBufferPointer() + b.GetSize()));

  // Quest 2 (prereq 1) must be unlocked by reconciliation.
  CHECK_EQ(lastAdvertisedStatus(pub, 2),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "reconciliation unlocks quest 2 after quest 1 forced COMPLETED");
  // Quest 3 (prereq 2) stays LOCKED: 2 is AVAILABLE, not COMPLETED yet.
  CHECK_EQ(lastAdvertisedStatus(pub, 3),
           static_cast<int>(quest::QuestStatus::LOCKED),
           "quest 3 stays LOCKED while quest 2 is only AVAILABLE");

  // Quest 2 forced COMPLETED → reconciliation unlocks quest 3.
  flatbuffers::FlatBufferBuilder b2(128);
  auto off2 = buildProgress(b2, player,
      {{2, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});
  b2.Finish(off2);
  mgr.loadProgress(player, std::vector<uint8_t>(b2.GetBufferPointer(),
                                                b2.GetBufferPointer() + b2.GetSize()));
  CHECK_EQ(lastAdvertisedStatus(pub, 3),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "reconciliation unlocks quest 3 after quest 2 forced COMPLETED");
  PASS();
}

// quest_graph.json is the source of truth for prerequisites: LoadGraph must
// overwrite the stale CSV prereq column (CSV quests 12/13 reference 13 as a
// self-dependency, JSON says 12→[11], 13→[11]). Regression for "quest 12
// stayed LOCKED although quest 11 was forced COMPLETED".
static void test_QuestData_graph_json_overrides_csv_prereqs() {
  quest::QuestData qd;
  CHECK(qd.LoadCSV(DATA_DIR "/quests/quests.csv"), "LoadCSV ok");
  CHECK(qd.LoadGraph(DATA_DIR "/quests/quest_graph.json"), "LoadGraph ok");

  auto prereqs12 = qd.GetPrerequisites(12);
  CHECK(prereqs12.size() == 1 && prereqs12[0] == 11,
        "quest 12 prereqs come from JSON (11), not stale CSV (13)");
  auto prereqs13 = qd.GetPrerequisites(13);
  CHECK(prereqs13.size() == 1 && prereqs13[0] == 11,
        "quest 13 prereqs come from JSON (11), not stale CSV self-loop (13)");
  PASS();
}

// End-to-end: with JSON prereqs, forcing quest 11 COMPLETED via loadProgress
// must unlock quest 12 (Heat Macerator) — the exact user-reported bug.
static void test_QuestManager_loadProgress_unlocks_quest_12() {
  const uint64_t player = 99;
  quest::QuestData qd;
  quest::QuestGraph graph;
  RecordingPublisher pub;
  simcore::QuestManager mgr(&qd, &graph, pub.callback());
  buildManager(qd, graph);

  mgr.onPlayerJoined(player);
  flatbuffers::FlatBufferBuilder b(128);
  auto off = buildProgress(b, player,
      {{11, static_cast<uint8_t>(quest::QuestStatus::COMPLETED), 100}});
  b.Finish(off);
  mgr.loadProgress(player, std::vector<uint8_t>(b.GetBufferPointer(),
                                                b.GetBufferPointer() + b.GetSize()));

  CHECK_EQ(lastAdvertisedStatus(pub, 12),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "quest 12 (Heat Macerator) unlocks when quest 11 forced COMPLETED");
  PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

void test_quest_manager() {
  TEST(QuestManager_completeQuest_valid_and_idempotent);
  TEST(QuestManager_completeQuest_rejects_invalid);
  TEST(QuestManager_completeQuest_rejects_exchange);
  TEST(QuestData_eraMap_covers_all_quests);
  TEST(QuestManager_completeQuest_unlocks_dependents);
  TEST(QuestManager_eraTransition_published_once);
  TEST(QuestManager_detection_one_step_craft);
  TEST(QuestManager_detection_block_placed);
  TEST(QuestManager_detection_side_configured);
  TEST(QuestManager_detection_inventory);
  TEST(QuestManager_rejoin_preserves_progress);
  TEST(QuestManager_loadProgress_reconciles_unlocks);
  TEST(QuestData_graph_json_overrides_csv_prereqs);
  TEST(QuestManager_loadProgress_unlocks_quest_12);
}
