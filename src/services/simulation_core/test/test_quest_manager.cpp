// QuestManager::completeQuest — manual (server-authoritative) quest completion.
// Uses the real data/quests/quests.csv + quest_graph.json (chain 1 → 2 → 3 …).
#include <libgtnh-net/test/test.h>

#include "Quest/QuestManager.h"
#include "quest_lib/QuestData.h"
#include "quest_lib/QuestGraph.h"
#include "quest_generated.h"
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
// Unlock propagation: completing quest 2 unlocks quest 3 (prereq met), which
// is advertised AVAILABLE and published on quest.unlocked.
// ─────────────────────────────────────────────────────────────────────────────
static void test_QuestManager_completeQuest_unlocks_dependents() {
  const uint64_t player = 99;
  QuestFixture fx(player);

  CHECK(fx.mgr.completeQuest(player, 2), "completeQuest(2) accepted");
  CHECK_EQ(countTopic(fx.pub, "quest.unlocked"), 1, "quest.unlocked published");
  CHECK_EQ(lastAdvertisedStatus(fx.pub, 3),
           static_cast<int>(quest::QuestStatus::AVAILABLE),
           "dependent quest 3 advertised AVAILABLE after prereq 2 completed");
  PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

void test_quest_manager() {
  TEST(QuestManager_completeQuest_valid_and_idempotent);
  TEST(QuestManager_completeQuest_rejects_invalid);
  TEST(QuestManager_completeQuest_unlocks_dependents);
}
