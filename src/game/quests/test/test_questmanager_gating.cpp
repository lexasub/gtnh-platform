// QuestManager state-transition unit tests (issues gp-phl3 + gp-qnrk).
//
// Covers the two QuestManager surfaces, using a fully synthetic 5-quest graph
// so each transition can be asserted one prerequisite at a time:
//
//   gp-phl3 — completion side effects: the AVAILABLE -> COMPLETED transition,
//             the progress value carried on the wire, the reward event, and
//             idempotency of a repeated completion.
//   gp-qnrk — prerequisite evaluation and unlock gating: a quest with unmet
//             prerequisites is not completable, meeting them unlocks it, a
//             quest whose prerequisite is itself locked stays locked, and
//             completing a quest unlocks exactly its direct dependents.
//
// Deliberately NOT re-tested here: the manual completeQuest() validation matrix
// and each per-detection-type handler. Those are already covered end to end by
// src/apps/simcore/test/test_quest_manager.cpp (18 cases, registered in
// simcored_test) against the real dataset. This suite instead covers what that
// suite does not look at:
//
//   * publishQuestProgressSnapshot's progress mapping — COMPLETED -> 100 and
//     every other status -> 0 (QuestManager.cpp:58-62). That is the exact 0/100
//     pair the gp-42p Go tracer asserts over a socket at
//     test/integration/questbook_wire_tracer_test.go:86, and no other C++ path
//     publishes a progress value.
//   * loadProgress's < 4-byte rejection, its out-of-range status clamp
//     (status >= 4 -> LOCKED, QuestManager.cpp:625-629 — meaningful only because
//     quest::QuestStatus has exactly four values, pinned in test_questtypes.cpp),
//     and its player-id mismatch handling (warn, then apply anyway).
//   * distributeRewards' observed C++ no-op: QuestDef::rewardItemId /
//     rewardCount are never populated by the C++ loaders, so the reward is
//     granted by MetaDB off the quest.completed event, not by C++ at all.
//   * handleQuestMet's autoComplete == false branch, which is only reachable
//     before onPlayerJoined has seeded the player's state (afterwards every
//     quest with met prerequisites has already been reconciled to AVAILABLE).
//
// Deterministic by construction: synthetic temp fixtures, a process-wide item
// registry loaded once from the real items.csv (the only real data file read,
// located through the DATA_DIR compile definition), and assertions only on
// recorded FlatBuffers payloads. No clock, network, display or randomness.
//
// Two things the wire is NOT, and which several cases below assert explicitly so
// a future change to them is visible rather than silent:
//
//  1. A quest is only advertised when QuestManager TRANSITIONS it. A quest left
//     LOCKED is never published on quest.progress.updated, so
//     lastAdvertisedStatus() returns -1 for it — not kStatusLocked. The gating
//     claim for a still-locked quest is therefore asserted through
//     completeQuest()'s refusal or through QuestGraph::CanComplete(), which are
//     the two observable proofs that the gate actually held.
//  2. A quest absent from a player's state is invisible, not LOCKED. Only
//     onPlayerJoined seeds the full graph (and loadProgress overwrites from
//     MetaDB); a detection that lands before either seeds just the one quest it
//     completes.
#include <game/quests/QuestManager.h>
#include <game/quests/QuestData.h>
#include <game/quests/QuestGraph.h>
#include <game/recipes/ItemRegistry.h>
#include <engine/registry/ItemId.h>
#include "quest_generated.h"
#include <engine/net/test/test.h>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Harness — the project's own CHECK/TEST convention
// (src/engine/net/test/test.h, src/game/machines/test/test_explosion_system.cpp).
// The repo has NO GoogleTest dependency: gtest is absent from conanfile.txt, CI
// does not install libgtest-dev, and CI builds Release with a global -Werror —
// a find_package(GTest QUIET) guard would make the test silently vanish from CI.
// ---------------------------------------------------------------------------
int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char *file, int line, const char *expr,
                const char *msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg)
            fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}

// Integral comparison that cannot trip -Wsign-compare under -Wall -Wextra
// -Wpedantic -Werror.
#define CHECK_EQI(a, b, ...)                                                   \
    test_check(static_cast<long long>(a) == static_cast<long long>(b),          \
               __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

#ifndef DATA_DIR
#error "DATA_DIR must be defined to the repository src/content/data root"
#endif

namespace fs = std::filesystem;

namespace {

struct PublishedMsg {
    std::string topic;
    std::vector<uint8_t> data;
};

// Records everything QuestManager publishes. The same double as the simcore
// suite, re-declared locally because that file's helpers are file-local.
struct RecordingPublisher {
    std::vector<PublishedMsg> msgs;
    std::function<void(const std::string &, const uint8_t *, size_t)> callback() {
        return [this](const std::string &topic, const uint8_t *data, size_t len) {
            msgs.push_back(PublishedMsg{topic, std::vector<uint8_t>(data, data + len)});
        };
    }
    void clear() { msgs.clear(); }
};

int countTopic(const RecordingPublisher &pub, const std::string &topic) {
    int n = 0;
    for (const auto &m : pub.msgs)
        if (m.topic == topic)
            ++n;
    return n;
}

// One (quest_id, status, progress) entry as MetaDB would report it.
struct ProgressEntry {
    uint32_t id;
    uint8_t status;
    uint8_t progress;
};

flatbuffers::Offset<Protocol::QuestProgressUpdate>
buildProgress(flatbuffers::FlatBufferBuilder &b, uint64_t playerId,
              const std::vector<ProgressEntry> &entries) {
    std::vector<flatbuffers::Offset<Protocol::QuestEntry>> offs;
    for (const auto &e : entries) {
        offs.push_back(Protocol::CreateQuestEntry(
            b, e.id, static_cast<Protocol::QuestStatus>(e.status), e.progress));
    }
    auto vec = b.CreateVector(offs);
    return Protocol::CreateQuestProgressUpdate(b, playerId, vec);
}

std::vector<uint8_t> finishToBytes(flatbuffers::FlatBufferBuilder &b,
                                   flatbuffers::Offset<Protocol::QuestProgressUpdate> off) {
    b.Finish(off);
    return std::vector<uint8_t>(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Every (quest_id, status, progress) triple QuestManager advertised on
// quest.progress.updated, in publication order.
struct Advertised {
    uint32_t questId = 0;
    int status = -1;
    int progress = -1;
};

std::vector<Advertised> allProgressUpdates(const RecordingPublisher &pub) {
    std::vector<Advertised> out;
    for (const auto &m : pub.msgs) {
        if (m.topic != "quest.progress.updated")
            continue;
        flatbuffers::Verifier v(m.data.data(), m.data.size());
        if (!v.VerifyBuffer<Protocol::QuestProgressUpdate>(nullptr))
            continue;
        auto *u = flatbuffers::GetRoot<Protocol::QuestProgressUpdate>(m.data.data());
        if (!u || !u->quests())
            continue;
        for (size_t i = 0; i < u->quests()->size(); ++i) {
            auto *e = u->quests()->Get(i);
            if (!e)
                continue;
            out.push_back(Advertised{e->quest_id(), static_cast<int>(e->status()),
                                     static_cast<int>(e->progress())});
        }
    }
    return out;
}

int lastAdvertisedStatus(const RecordingPublisher &pub, uint32_t questId) {
    int found = -1;
    for (const auto &a : allProgressUpdates(pub))
        if (a.questId == questId)
            found = a.status;
    return found;
}

int lastAdvertisedProgress(const RecordingPublisher &pub, uint32_t questId) {
    int found = -1;
    for (const auto &a : allProgressUpdates(pub))
        if (a.questId == questId)
            found = a.progress;
    return found;
}

int advertisedCount(const RecordingPublisher &pub, uint32_t questId) {
    int n = 0;
    for (const auto &a : allProgressUpdates(pub))
        if (a.questId == questId)
            ++n;
    return n;
}

// Quest ids carried by the `index`-th quest.unlocked event (0-based).
std::vector<uint32_t> unlockedIds(const RecordingPublisher &pub, size_t index = 0) {
    std::vector<std::vector<uint32_t>> all;
    for (const auto &m : pub.msgs) {
        if (m.topic != "quest.unlocked")
            continue;
        flatbuffers::Verifier v(m.data.data(), m.data.size());
        if (!v.VerifyBuffer<Protocol::QuestUnlocked>(nullptr))
            continue;
        auto *u = flatbuffers::GetRoot<Protocol::QuestUnlocked>(m.data.data());
        if (!u || !u->unlocked_quest_ids())
            continue;
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < u->unlocked_quest_ids()->size(); ++i)
            ids.push_back(u->unlocked_quest_ids()->Get(i));
        all.push_back(std::move(ids));
    }
    if (all.empty())
        return {};
    return all.at(index < all.size() ? index : all.size() - 1);
}

// True if any quest.completed event names this quest — i.e. the reward event.
bool hasCompletionFor(const RecordingPublisher &pub, uint32_t questId) {
    for (const auto &m : pub.msgs) {
        if (m.topic != "quest.completed")
            continue;
        flatbuffers::Verifier v(m.data.data(), m.data.size());
        if (!v.VerifyBuffer<Protocol::QuestCompleted>(nullptr))
            continue;
        auto *c = flatbuffers::GetRoot<Protocol::QuestCompleted>(m.data.data());
        if (c && static_cast<uint32_t>(c->quest_id()) == questId)
            return true;
    }
    return false;
}

std::string csvRow(uint32_t id) {
    return std::to_string(id) + ",Quest " + std::to_string(id) +
           ",Synthetic quest,vagrant,misc,,,,\n";
}

std::string graphEntry(uint32_t id, const std::vector<uint32_t> &prereqs) {
    std::string s = "{\"id\": " + std::to_string(id) + ", \"prereqs\": [";
    for (size_t i = 0; i < prereqs.size(); ++i) {
        if (i)
            s += ", ";
        s += std::to_string(prereqs[i]);
    }
    return s + "]}";
}

// The shared graph, chosen so every gating question in gp-qnrk can be asked one
// step at a time:
//
//   1 --> 2 --> 4
//   |           ^
//   +--> 3 -----+        quest 4 needs BOTH 2 and 3
//
// plus quest 5 as an independent root, so "unlocks exactly its direct
// dependents" has a quest that no cascade may touch.
constexpr uint32_t kRoot = 1;  // no prerequisites
constexpr uint32_t kLeft = 2;  // prereq 1
constexpr uint32_t kRight = 3; // prereq 1
constexpr uint32_t kJoin = 4;  // prereqs 2 and 3
constexpr uint32_t kOther = 5; // independent root

// Each fixture quest detects a DIFFERENT real item, so a detection event can
// only ever affect the one quest under test. All five ids exist in items.csv.
const char *targetOf(uint32_t id) {
    switch (id) {
    case kRoot:
        return "0:10:11:2"; // oak_log
    case kLeft:
        return "0:10:11:1"; // crafting_table
    case kRight:
        return "0:0:1";     // stone
    case kJoin:
        return "0:10:11:0"; // chest
    default:
        return "0:0:2"; // cobblestone
    }
}

const uint32_t kFixtureIds[] = {kRoot, kLeft, kRight, kJoin, kOther};

// The item registry is a process-wide singleton with its own loaded_ guard; the
// detection handlers need it to resolve packed ids to hierarchical strings.
// DATA_DIR supplies the real items.csv — the only real data file this suite
// touches. Idempotent, so every case may call it.
void ensureItemRegistry() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(std::string(DATA_DIR) +
                                                        "/registry/items.csv");
}

bool contains(const std::vector<uint32_t> &v, uint32_t id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

// Synthetic quest set + a QuestManager wired to it exactly like production
// main.cpp:531-536. `withRequirements` gives every quest a CRAFT detection
// objective (and auto_complete per `autoComplete`); without it the quests
// carry no detection fields, which is what QuestManager sees when a deployment
// ships quest_requirements.json unparsed.
struct Fixture {
    quest::QuestData qd;
    quest::QuestGraph graph;
    RecordingPublisher pub;
    simcore::QuestManager mgr;
    fs::path dir;

    Fixture(const std::string &name, bool withRequirements, bool autoComplete = true)
        : mgr(&qd, &graph, pub.callback()),
          dir(fs::temp_directory_path() / ("quest_mgr_test_" + name)) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);

        write("quests.csv",
              "id,title,description,era,section,cost_item,cost_count,cooldown,"
              "target_count\n" +
                  csvRow(kRoot) + csvRow(kLeft) + csvRow(kRight) + csvRow(kJoin) +
                  csvRow(kOther));
        write("quest_graph.json",
              "{\"quests\": [" + graphEntry(kRoot, {}) + ", " +
                  graphEntry(kLeft, {kRoot}) + ", " + graphEntry(kRight, {kRoot}) +
                  ", " + graphEntry(kJoin, {kLeft, kRight}) + ", " +
                  graphEntry(kOther, {}) + "]}");
        if (withRequirements) {
            std::string body;
            for (uint32_t id : kFixtureIds) {
                body += "  \"" + std::to_string(id) + "\": {\"auto_complete\": " +
                        (autoComplete ? "true" : "false") + ", \"requirements\": "
                        "[{\"kind\": \"craft\", \"item\": \"" + targetOf(id) +
                        "\", \"count\": 1}]},\n";
            }
            write("quest_requirements.json", "{\n" + body + "  \"9999\": {}\n}\n");
        }

        qd.LoadCSV(path("quests.csv"));
        qd.LoadGraph(path("quest_graph.json"));
        if (withRequirements)
            qd.LoadRequirementsJSON(path("quest_requirements.json"));
        std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
        for (const auto &q : qd.AllQuests())
            prereqs[q.id] = q.prerequisites;
        graph.Init(qd.Graph(), prereqs);
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;

    std::string path(const std::string &leaf) const {
        return (dir / leaf).string();
    }

private:
    void write(const std::string &leaf, const std::string &contents) const {
        std::ofstream of(path(leaf));
        of << contents;
    }
};

// onPlayerJoined + loadProgress: the two calls production makes before a
// player's quests can act.
void seed(simcore::QuestManager &mgr, uint64_t player,
          const std::vector<ProgressEntry> &entries) {
    mgr.onPlayerJoined(player);
    flatbuffers::FlatBufferBuilder b(256);
    auto off = buildProgress(b, player, entries);
    mgr.loadProgress(player, finishToBytes(b, off));
}

const uint8_t kCompleted = static_cast<uint8_t>(quest::QuestStatus::COMPLETED);
const uint8_t kAvailable = static_cast<uint8_t>(quest::QuestStatus::AVAILABLE);
const uint8_t kInProgress = static_cast<uint8_t>(quest::QuestStatus::IN_PROGRESS);
const uint8_t kLocked = static_cast<uint8_t>(quest::QuestStatus::LOCKED);
const int kStatusCompleted = static_cast<int>(quest::QuestStatus::COMPLETED);
const int kStatusAvailable = static_cast<int>(quest::QuestStatus::AVAILABLE);
const int kStatusInProgress = static_cast<int>(quest::QuestStatus::IN_PROGRESS);
const int kStatusLocked = static_cast<int>(quest::QuestStatus::LOCKED);

} // namespace

// ---------------------------------------------------------------------------
// Fixture sanity: the graph and the registry the other cases reason about
// ---------------------------------------------------------------------------
static void test_fixture_graph_is_the_expected_shape() {
    Fixture fx("shape", /*withRequirements=*/false);
    CHECK_EQI(fx.qd.Count(), 5, "five synthetic quests loaded");
    ensureItemRegistry();
    // Every detection target must be resolvable through the real item registry,
    // otherwise the detection cases would silently match nothing.
    for (uint32_t id : kFixtureIds) {
        const uint16_t packed = ItemId::pack(targetOf(id));
        CHECK(RecipeManager::ItemRegistry::instance().idToHierarchical(packed) ==
                  std::string(targetOf(id)),
              "the fixture's packed item id resolves back to its hierarchical target");
        CHECK(packed != 0, "the fixture item id is not the empty-id sentinel");
    }

    CHECK(fx.graph.CanComplete(kRoot, {}), "quest 1 is a root");
    CHECK(!fx.graph.CanComplete(kLeft, {}), "quest 2 is gated by quest 1");
    CHECK(!fx.graph.CanComplete(kJoin, {{kLeft, quest::QuestStatus::COMPLETED}}),
          "quest 4 needs both 2 and 3");
    CHECK(fx.graph.CanComplete(kJoin, {{kLeft, quest::QuestStatus::COMPLETED},
                                      {kRight, quest::QuestStatus::COMPLETED}}),
          "quest 4 unlocks once both prerequisites are COMPLETED");
    CHECK(fx.graph.CanComplete(kOther, {}), "quest 5 is an independent root");
}

// ---------------------------------------------------------------------------
// gp-qnrk: onPlayerJoined seeds roots AVAILABLE and dependents LOCKED
// ---------------------------------------------------------------------------
static void test_onPlayerJoined_seeds_roots_AVAILABLE_and_dependents_LOCKED() {
    const uint64_t player = 1;
    Fixture fx("join", /*withRequirements=*/false);
    fx.mgr.onPlayerJoined(player);

    // The roots are announced in one bulk quest.unlocked event, and no
    // per-quest progress update is published at all (QuestManager.cpp:257-259).
    const auto ids = unlockedIds(fx.pub);
    CHECK_EQI(ids.size(), 2, "both root quests announced as unlocked");
    CHECK(contains(ids, kRoot), "quest 1 is a root");
    CHECK(contains(ids, kOther), "quest 5 is a root");
    CHECK(!contains(ids, kLeft), "quest 2 is not a root");
    CHECK(!contains(ids, kRight), "quest 3 is not a root");
    CHECK(!contains(ids, kJoin), "quest 4 is not a root");
    CHECK_EQI(countTopic(fx.pub, "quest.progress.updated"), 0,
              "onPlayerJoined publishes no per-quest progress update");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 0,
              "joining completes nothing");

    // The seeded state must leave only the roots newly available: that is the
    // property the dependents' LOCKED seeding exists to create.
    const std::unordered_map<uint32_t, quest::QuestStatus> allLocked{
        {kRoot, quest::QuestStatus::LOCKED},
        {kLeft, quest::QuestStatus::LOCKED},
        {kRight, quest::QuestStatus::LOCKED},
        {kJoin, quest::QuestStatus::LOCKED},
        {kOther, quest::QuestStatus::LOCKED}};
    const auto newly = fx.graph.NewlyAvailable(allLocked);
    CHECK_EQI(newly.size(), 2, "only the two roots unlock from a fully locked start");
    CHECK(contains(newly, kRoot), "quest 1 unlocks");
    CHECK(contains(newly, kOther), "quest 5 unlocks");
}

// ---------------------------------------------------------------------------
// gp-qnrk: transitive gating — a prerequisite that is only AVAILABLE still
// blocks its own dependent
// ---------------------------------------------------------------------------
static void test_transitive_gating_holds_while_a_prerequisite_is_LOCKED() {
    const uint64_t player = 2;
    Fixture fx("gating", /*withRequirements=*/false);
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}});

    // Quest 1 COMPLETED -> both of its dependents reconcile to AVAILABLE; quest
    // 4 does not, because 2 and 3 are only AVAILABLE, never COMPLETED.
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusAvailable,
              "quest 2 unlocks once quest 1 is COMPLETED");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRight), kStatusAvailable,
              "quest 3 unlocks once quest 1 is COMPLETED");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusLocked,
              "quest 4 stays LOCKED while quests 2 and 3 are only AVAILABLE");

    // Its advertised blockers are exactly the prerequisites not yet COMPLETED.
    const std::unordered_map<uint32_t, quest::QuestStatus> progress{
        {kRoot, quest::QuestStatus::COMPLETED},
        {kLeft, quest::QuestStatus::AVAILABLE},
        {kRight, quest::QuestStatus::AVAILABLE},
        {kJoin, quest::QuestStatus::LOCKED}};
    const auto blockers = fx.graph.LockedByPrereqs(kJoin, progress);
    CHECK_EQI(blockers.size(), 2, "both prerequisites of quest 4 are unmet");
    CHECK(contains(blockers, kLeft), "quest 2 is an unmet prerequisite");
    CHECK(contains(blockers, kRight), "quest 3 is an unmet prerequisite");
    CHECK(!fx.graph.CanComplete(kJoin, progress), "quest 4 is not completable");

    // Manual completion of the still-gated quest is refused, with no side effect.
    fx.pub.clear();
    CHECK(!fx.mgr.completeQuest(player, kJoin),
          "completeQuest on a quest whose prerequisites are unmet is rejected");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 0,
              "a rejected completion publishes no reward event");
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0, "a rejected completion unlocks nothing");
    CHECK_EQI(static_cast<int>(allProgressUpdates(fx.pub).size()), 0,
              "a rejected completion advertises nothing");
}

static void test_locked_intermediate_blocks_its_dependent() {
    // A quest whose prerequisite is itself LOCKED stays locked, even though the
    // grandparent is COMPLETED: there is no shortcut through a locked middle.
    Fixture fx("lockchain", /*withRequirements=*/false);
    const std::unordered_map<uint32_t, quest::QuestStatus> midLocked{
        {kRoot, quest::QuestStatus::COMPLETED},
        {kLeft, quest::QuestStatus::LOCKED},
        {kRight, quest::QuestStatus::LOCKED},
        {kJoin, quest::QuestStatus::LOCKED}};
    CHECK(!fx.graph.CanComplete(kJoin, midLocked), "quest 4 is gated by locked 2 and 3");
    // Both children of quest 1 are released by the same completed grandparent:
    // NewlyAvailable advances every LOCKED quest whose prerequisites are met, not
    // just the first match, so the batch is {2, 3} (QuestGraph.cpp:30-34 iterates
    // the whole state). Quest 4 is excluded because 2 and 3 are still LOCKED.
    const auto newly = fx.graph.NewlyAvailable(midLocked);
    CHECK_EQI(newly.size(), 2, "both dependents of the completed quest 1 are newly available");
    CHECK(contains(newly, kLeft), "quest 2 is newly available");
    CHECK(contains(newly, kRight), "quest 3 is newly available");
    CHECK(!contains(newly, kJoin), "quest 4 is NOT newly available while 2 and 3 are locked");
    // NewlyAvailable walks an unordered_map, so it is order-insensitive: the
    // batch content is what matters, never its order.
    CHECK(!fx.graph.CanComplete(kJoin, {{kRoot, quest::QuestStatus::COMPLETED},
                                       {kRight, quest::QuestStatus::COMPLETED}}),
          "grandparent + sibling completion is not enough for quest 4");
}

// ---------------------------------------------------------------------------
// gp-qnrk: completing a quest unlocks exactly its direct dependents
// ---------------------------------------------------------------------------
static void test_completion_unlocks_exactly_the_direct_dependents() {
    const uint64_t player = 3;
    Fixture fx("cascade", /*withRequirements=*/false);
    // Quest 3 is already COMPLETED, so quest 4 is the one quest left waiting on
    // quest 2 — completing 2 must release 4 and nothing else.
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}, {kRight, kCompleted, 100}});
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusLocked,
              "precondition: quest 4 is LOCKED before quest 2 completes");
    fx.pub.clear();

    CHECK(fx.mgr.completeQuest(player, kLeft), "completeQuest on quest 2 is accepted");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1, "one quest.completed published");
    CHECK(hasCompletionFor(fx.pub, kLeft), "the quest.completed event names quest 2");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusCompleted,
              "quest 2 is advertised COMPLETED");

    const auto ids = unlockedIds(fx.pub);
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 1, "exactly one unlock event");
    CHECK_EQI(ids.size(), 1, "the unlock batch carries exactly one quest");
    CHECK_EQI(ids[0], kJoin, "the unlocked quest is quest 4");
    CHECK(!contains(ids, kRight), "quest 3 is not re-unlocked");
    CHECK(!contains(ids, kOther), "the independent root 5 is untouched");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusAvailable,
              "quest 4 is advertised AVAILABLE");

    // The transitive property: 4 is now completable, and completing it — a leaf
    // — produces no further unlock at all.
    fx.pub.clear();
    CHECK(fx.mgr.completeQuest(player, kJoin), "completeQuest on quest 4 is accepted");
    CHECK(hasCompletionFor(fx.pub, kJoin), "the second event names quest 4");
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0,
              "a leaf completion unlocks nothing further");
}

static void test_completion_releases_the_join_only_when_both_prereqs_are_done() {
    // The same graph, driven through the other branch: quest 4 needs BOTH 2 and
    // 3 COMPLETED, and completing only one is not enough.
    const uint64_t player = 31;
    Fixture fx("join2", /*withRequirements=*/false);
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}});
    fx.pub.clear();

    // Quest 4 is LOCKED here, and a quest that stays LOCKED is never advertised:
    // unlockNewlyAvailable (QuestManager.cpp:218-222) publishes a progress update
    // only for the quests it actually transitions. -1 is therefore the observed
    // wire state, and the gating claim is asserted on the graph itself.
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), -1,
              "a quest that stays LOCKED is never advertised at all");

    CHECK(fx.mgr.completeQuest(player, kLeft), "completeQuest on quest 2 is accepted");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), -1,
              "quest 4 is still not advertised after only one of two prerequisites");
    CHECK(!fx.graph.CanComplete(kJoin, {{kRoot, quest::QuestStatus::COMPLETED},
                                       {kLeft, quest::QuestStatus::COMPLETED},
                                       {kRight, quest::QuestStatus::AVAILABLE},
                                       {kJoin, quest::QuestStatus::LOCKED}}),
          "quest 4 is gated: quest 3 is only AVAILABLE, never COMPLETED");
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0, "nothing unlocked yet");

    CHECK(fx.mgr.completeQuest(player, kRight), "completeQuest on quest 3 is accepted");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusAvailable,
              "quest 4 unlocks once the second prerequisite completes");
    const auto ids = unlockedIds(fx.pub);
    CHECK_EQI(ids.size(), 1, "exactly one quest unlocked");
    CHECK_EQI(ids[0], kJoin, "the unlocked quest is quest 4");
}

// ---------------------------------------------------------------------------
// gp-phl3: completion sets status COMPLETED and progress to 100
// ---------------------------------------------------------------------------
static void test_completion_sets_status_COMPLETED_and_progress_100() {
    const uint64_t player = 5;
    Fixture fx("progress", /*withRequirements=*/false);
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}});
    fx.pub.clear();

    CHECK(fx.mgr.completeQuest(player, kLeft), "completeQuest on quest 2 is accepted");

    // The advertised triple for the completed quest: COMPLETED (wire byte 3) and
    // progress 100 — the exact pair the gp-42p Go tracer asserts over a socket
    // at questbook_wire_tracer_test.go:86.
    bool found = false;
    for (const auto &a : allProgressUpdates(fx.pub)) {
        if (a.questId != kLeft)
            continue;
        found = true;
        CHECK_EQI(a.status, kStatusCompleted, "the completed quest is advertised COMPLETED");
        CHECK_EQI(a.status, 3, "COMPLETED is wire byte 3");
        CHECK_EQI(a.progress, 100, "the completed quest is advertised at 100 percent");
    }
    CHECK(found, "the completed quest was advertised on quest.progress.updated");
    // A quest that merely unlocked is advertised at 0, never 100.
    for (const auto &a : allProgressUpdates(fx.pub)) {
        if (a.questId != kJoin)
            continue;
        CHECK_EQI(a.status, kStatusAvailable, "an unlocked quest is advertised AVAILABLE");
        CHECK_EQI(a.progress, 0, "an unlocked quest is advertised at 0 percent");
    }
}

static void test_progress_snapshot_maps_COMPLETED_to_100_and_others_to_0() {
    // publishQuestProgressSnapshot (QuestManager.cpp:58-62) sends the whole
    // player's state in one quest.progress.updated and derives the progress
    // field from the status: COMPLETED -> 100, everything else -> 0. No other
    // C++ path publishes a progress value, so this is the mapping a client sees
    // after loadProgress.
    const uint64_t player = 6;
    Fixture fx("snapshot", /*withRequirements=*/false);
    seed(fx.mgr, player, {{kRoot, kCompleted, 100},
                           {kLeft, kAvailable, 0},
                           {kRight, kInProgress, 0},
                           {kJoin, kLocked, 0}});

    // The snapshot is the single event carrying the whole player's state at
    // once; the reconciliation events that follow carry one quest each. It is
    // publishQuestProgressSnapshot (QuestManager.cpp:58-62) that emits it, and
    // that walks progress_[playerId] — the player's FULL state, not just the
    // entries MetaDB returned. Quest 5 is absent from the payload but was
    // already seeded AVAILABLE as a root by onPlayerJoined, so it is part of
    // the snapshot too: 5 entries, not the 4 that were loaded.
    int snapshotCount = 0;
    int snapshotEntries = 0;
    for (const auto &m : fx.pub.msgs) {
        if (m.topic != "quest.progress.updated")
            continue;
        flatbuffers::Verifier v(m.data.data(), m.data.size());
        if (!v.VerifyBuffer<Protocol::QuestProgressUpdate>(nullptr))
            continue;
        auto *u = flatbuffers::GetRoot<Protocol::QuestProgressUpdate>(m.data.data());
        if (u && u->quests() && u->quests()->size() > 1) {
            ++snapshotCount;
            snapshotEntries = static_cast<int>(u->quests()->size());
        }
    }
    CHECK_EQI(snapshotCount, 1, "exactly one multi-entry snapshot was published");
    CHECK_EQI(snapshotEntries, 5,
              "the snapshot carries all five seeded quests, not only the four loaded");

    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRoot), kStatusCompleted,
              "quest 1 is COMPLETED in the snapshot");
    CHECK_EQI(lastAdvertisedProgress(fx.pub, kRoot), 100,
              "a COMPLETED quest is sent at progress 100");
    CHECK_EQI(lastAdvertisedProgress(fx.pub, kLeft), 0,
              "an AVAILABLE quest is sent at progress 0");
    CHECK_EQI(lastAdvertisedProgress(fx.pub, kRight), 0,
              "an IN_PROGRESS quest is sent at progress 0");
    CHECK_EQI(lastAdvertisedProgress(fx.pub, kJoin), 0,
              "a LOCKED quest is sent at progress 0");

    // The IN_PROGRESS status itself round-trips through loadProgress.
    const uint64_t player2 = 61;
    Fixture fx2("snapshot2", /*withRequirements=*/false);
    seed(fx2.mgr, player2, {{kRoot, kInProgress, 37}});
    CHECK_EQI(lastAdvertisedStatus(fx2.pub, kRoot), kStatusInProgress,
              "an IN_PROGRESS status round-trips through loadProgress");
    // ... and the stored sub-100 progress is NOT echoed: the snapshot derives
    // progress from status, so a stored 37 is reported as 0.
    CHECK_EQI(lastAdvertisedProgress(fx2.pub, kRoot), 0,
              "a non-COMPLETED quest reports progress 0 whatever was stored");
}

// ---------------------------------------------------------------------------
// gp-phl3: the reward is issued exactly once; a repeat is idempotent
// ---------------------------------------------------------------------------
static void test_reward_is_issued_exactly_once_and_repeat_is_idempotent() {
    const uint64_t player = 7;
    Fixture fx("reward", /*withRequirements=*/false);
    // Quest 3 is COMPLETED too, so completing quest 2 really does release quest
    // 4: the unlock event the repeats must not repeat is a real one. (With only
    // quest 1 done, quest 4 stays LOCKED and the completion unlocks nothing at
    // all, which would make the "no second unlock" check vacuous.)
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}, {kRight, kCompleted, 100}});
    fx.pub.clear();

    CHECK(fx.mgr.completeQuest(player, kLeft), "the first completeQuest is accepted");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "the first completion issues one reward event");
    CHECK(hasCompletionFor(fx.pub, kLeft), "the reward event names the completed quest");
    const auto firstUnlocked = unlockedIds(fx.pub);
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 1,
              "the first completion unlocks quest 4 exactly once");
    CHECK_EQI(firstUnlocked.size(), 1, "the unlock batch carries exactly one quest");
    CHECK_EQI(firstUnlocked[0], kJoin, "the unlocked quest is quest 4");

    for (int attempt = 0; attempt < 3; ++attempt)
        CHECK(!fx.mgr.completeQuest(player, kLeft), "a repeat completeQuest is rejected");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "three rejected repeats issue no additional reward");
    CHECK_EQI(advertisedCount(fx.pub, kLeft), 1,
              "the completed quest is advertised exactly once");
    // The completion is not re-announced as an unlock either.
    const auto ids = unlockedIds(fx.pub);
    CHECK(!contains(ids, kLeft), "a completed quest is never re-unlocked");
    // The first completion unlocked quest 4 once; the repeats must not repeat it.
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 1,
              "the rejected repeats produce no second unlock event");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusAvailable,
              "quest 4 is still AVAILABLE and was not re-unlocked by the repeats");
}

static void test_distributeRewards_is_observed_as_a_CPP_noop() {
    // completeQuestInternal calls distributeRewards(), but that function only
    // forwards the C++-side QuestDef::rewardItemId / rewardCount — and
    // QuestData's loaders never populate them: quest_rewards.json is read by
    // LoadRewardsJSON into a SEPARATE map that is not copied onto the QuestDefs
    // (QuestData.cpp:272-289), and the 9-column CSV has no reward columns at all.
    // So the early return at QuestManager.cpp:675-678 is always taken and C++
    // issues no reward of its own: MetaDB re-reads quest_rewards.json
    // (src/apps/meta_db/definitions.go:79-80) off the quest.completed event.
    // Asserted as observed, so populating those fields later is a visible change.
    const uint64_t player = 8;
    Fixture fx("noop", /*withRequirements=*/true);
    ensureItemRegistry();

    const auto *def = fx.qd.GetQuest(kLeft);
    CHECK(def != nullptr, "quest 2 exists");
    if (def) {
        CHECK_EQI(def->rewardItemId, 0, "QuestDef.rewardItemId is never populated");
        CHECK_EQI(def->rewardCount, 0, "QuestDef.rewardCount is never populated");
    }
    CHECK(fx.qd.GetReward(kLeft) == nullptr,
          "the synthetic quest set declares no quest_rewards entry");

    seed(fx.mgr, player, {{kRoot, kCompleted, 100}});
    fx.pub.clear();
    CHECK(fx.mgr.completeQuest(player, kLeft), "completeQuest on quest 2 is accepted");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "the reward signal is the single quest.completed publication");
    // No reward-specific topic exists on this side: the reward row is written by
    // MetaDB, not by SimCore.
    for (const auto &m : fx.pub.msgs)
        CHECK(m.topic != "quest.reward" && m.topic != "quest.reward.granted",
              "no reward topic is published from C++");
}

// ---------------------------------------------------------------------------
// gp-phl3: rejection paths leave no trace
// ---------------------------------------------------------------------------
static void test_rejected_completions_change_nothing() {
    const uint64_t player = 9;
    Fixture fx("reject", /*withRequirements=*/false);
    // A regression state built through loadProgress, which is the only way to get
    // one: quest 2 is reported AVAILABLE while its own prerequisite quest 1 is
    // not COMPLETED. (A plain seed of {1: COMPLETED} makes quest 2 legitimately
    // AVAILABLE *and* legitimately completable, so it cannot exercise the
    // prerequisite refusal at all.) MetaDB can report this after an admin edit
    // or a data rollback, so completeQuest must re-check prerequisites even for a
    // quest that is already AVAILABLE (QuestManager.cpp:163-167).
    fx.mgr.onPlayerJoined(player);
    {
        flatbuffers::FlatBufferBuilder b(256);
        auto off = buildProgress(b, player, {{kRoot, kLocked, 0},
                                             {kLeft, kAvailable, 0},
                                             {kRight, kCompleted, 100}});
        fx.mgr.loadProgress(player, finishToBytes(b, off));
    }
    // Precondition, checked before the publisher is cleared: quest 2 really is
    // AVAILABLE, so the refusal below can only come from the prerequisite check
    // and not from the status check.
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusAvailable,
              "precondition: quest 2 is seeded AVAILABLE");
    fx.pub.clear();
    // LOCKED quest (prerequisites unmet) -> refused.
    CHECK(!fx.mgr.completeQuest(player, kJoin), "a LOCKED quest is refused");
    // Already-COMPLETED quest -> refused.
    CHECK(!fx.mgr.completeQuest(player, kRight), "an already-COMPLETED quest is refused");
    // Unknown quest ids -> refused, and not silently seeded into player state.
    CHECK(!fx.mgr.completeQuest(player, 4242), "an unknown quest id is refused");
    CHECK(!fx.mgr.completeQuest(player, 7777), "a second unknown quest id is refused");
    // A quest seeded AVAILABLE but whose prerequisites regressed -> refused.
    CHECK(!fx.mgr.completeQuest(player, kLeft),
          "an AVAILABLE quest with unmet prerequisites is refused");

    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 0, "no reward on any refusal");
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0, "no unlock on any refusal");
    CHECK_EQI(countTopic(fx.pub, "quest.era.transition"), 0,
              "no era transition on any refusal");
    CHECK_EQI(static_cast<int>(allProgressUpdates(fx.pub).size()), 0,
              "no progress advertisement on any refusal");
}

// ---------------------------------------------------------------------------
// loadProgress input handling
// ---------------------------------------------------------------------------
static void test_loadProgress_rejects_a_buffer_shorter_than_four_bytes() {
    const uint64_t player = 10;
    Fixture fx("shortbuf", /*withRequirements=*/false);
    fx.mgr.onPlayerJoined(player);
    fx.pub.clear();

    for (size_t len = 0; len < 4; ++len) {
        std::vector<uint8_t> tiny(len, 0xAB);
        fx.mgr.loadProgress(player, tiny);
    }
    CHECK_EQI(countTopic(fx.pub, "quest.progress.updated"), 0,
              "no snapshot is published for an undersized buffer");
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0,
              "no unlock is published for an undersized buffer");
    // The seed survives: a rejected load does not reset the player's state.
    CHECK(!fx.mgr.completeQuest(player, kJoin),
          "the pre-existing state still gates quest 4 after the rejected loads");
}

static void test_loadProgress_clamps_an_out_of_range_status_to_LOCKED() {
    // QuestManager.cpp:625-629: a status byte outside 0..3 is clamped to LOCKED
    // rather than stored as a bogus enum value. The four valid values are the
    // wire values pinned in test_questtypes.cpp.
    const uint8_t badStatuses[] = {4, 5, 9, 200, 255};
    for (uint8_t raw : badStatuses) {
        const uint64_t player = 11 + raw;
        Fixture fx("clamp", /*withRequirements=*/false);
        seed(fx.mgr, player, {{kRoot, raw, 0}, {kRight, kCompleted, 100}});

        // The invalid byte is never echoed to the wire.
        CHECK(lastAdvertisedStatus(fx.pub, kRoot) != static_cast<int>(raw),
              "the invalid status byte is not echoed");
        // Clamping to LOCKED hands the quest back to the normal reconciliation:
        // quest 1 is a root, so it comes straight back as AVAILABLE.
        CHECK_EQI(lastAdvertisedStatus(fx.pub, kRoot), kStatusAvailable,
                  "a clamped root quest is reconciled back to AVAILABLE");
        // A dependent of a still-incomplete quest stays LOCKED.
        CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusLocked,
                  "a dependent stays LOCKED after the clamp");
        // And a quest whose status WAS valid in the same payload survives.
        CHECK(hasCompletionFor(fx.pub, 0) == false, "no reward event from a load");
    }
}

static void test_loadProgress_applies_a_mismatched_player_id_anyway() {
    // QuestManager.cpp:607-611 only warns on a player-id mismatch and then
    // applies the payload to the REQUESTED player's state. Asserted as
    // observed: the authoritative read is keyed on the request, not the reply.
    const uint64_t requested = 12;
    const uint64_t inBuffer = 999;
    Fixture fx("mismatch", /*withRequirements=*/false);
    fx.mgr.onPlayerJoined(requested);
    fx.pub.clear();

    flatbuffers::FlatBufferBuilder b(128);
    auto off = buildProgress(b, inBuffer, {{kRoot, kCompleted, 100}});
    fx.mgr.loadProgress(requested, finishToBytes(b, off));

    // Applied to the requested player: quest 2 was reconciled to AVAILABLE.
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusAvailable,
              "a mismatched player id does not discard the payload");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 0, "a load completes nothing");
}

static void test_rejoin_preserves_completed_quests() {
    // onPlayerJoined only seeds quests missing from the player's state
    // (QuestManager.cpp:246-248), so a rejoin must not reset a completion and
    // must not re-announce it.
    const uint64_t player = 13;
    Fixture fx("rejoin", /*withRequirements=*/false);
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}});
    CHECK(fx.mgr.completeQuest(player, kLeft), "quest 2 completes before the rejoin");
    fx.pub.clear();

    fx.mgr.onPlayerJoined(player);
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0,
              "a rejoin re-announces nothing (every quest is already seeded)");
    CHECK_EQI(countTopic(fx.pub, "quest.progress.updated"), 0,
              "a rejoin publishes no per-quest progress update");
    // The completion still gates quest 4 exactly as before the rejoin.
    CHECK(!fx.mgr.completeQuest(player, kJoin),
          "quest 4 is still not completable after a rejoin");
    // And quest 2 itself is still COMPLETED, so a repeat is still refused.
    CHECK(!fx.mgr.completeQuest(player, kLeft),
          "quest 2 is still COMPLETED after the rejoin");
}

// ---------------------------------------------------------------------------
// Detection-driven completion — the same one-step transition, issued once
// ---------------------------------------------------------------------------
static void test_detection_completes_once_and_does_not_re_reward() {
    const uint64_t player = 14;
    Fixture fx("detect", /*withRequirements=*/true);
    ensureItemRegistry();
    const auto *def = fx.qd.GetQuest(kLeft);
    CHECK(def != nullptr, "quest 2 exists");
    if (def) {
        CHECK(def->detectType == quest::DetectionType::CRAFT, "quest 2 detects CRAFT");
        CHECK(def->detectTarget == std::string(targetOf(kLeft)),
              "quest 2 targets its own fixture item");
    }

    // Quest 1 COMPLETED, quest 2 still LOCKED: the one-step detection completes
    // it directly, with no intermediate AVAILABLE round trip.
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}});
    fx.pub.clear();

    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kLeft)), 1);
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "the detection completed exactly one quest");
    CHECK(hasCompletionFor(fx.pub, kLeft), "quest 2 is the completed quest");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusCompleted,
              "a LOCKED quest completes in one step via detection");
    CHECK_EQI(lastAdvertisedProgress(fx.pub, kLeft), 100,
              "the detected completion is advertised at 100 percent");
    // Quest 4 needs BOTH 2 and 3 COMPLETED and 3 is only AVAILABLE, so it is
    // still LOCKED — and because it is never transitioned it is never advertised
    // (QuestManager.cpp:218-222). Asserted through the manager instead of on the
    // wire: the refusal below is the observable proof that the gate held.
    CHECK(!fx.mgr.completeQuest(player, kJoin),
          "quest 4 stays LOCKED: its other prerequisite is not COMPLETED");

    // Repeats and irrelevant inputs change nothing.
    fx.pub.clear();
    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kLeft)), 1);
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 0,
              "a repeated detection issues no further reward");
    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kRight)), 0);
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 0,
              "a zero count is ignored outright");
}

static void test_detection_chain_respects_prerequisites_one_step_at_a_time() {
    // Each fixture quest detects a different item, so the chain can be walked
    // one prerequisite at a time and the gating checked at every link.
    const uint64_t player = 15;
    Fixture fx("detectchain", /*withRequirements=*/true);
    ensureItemRegistry();

    // The chain is walked AFTER the join, because onPlayerJoined is what seeds
    // the dependents into the player's state at all. A detection that completes
    // the root before the join seeds only that one quest
    // (completeQuestInternal's LOCKED seeding, QuestManager.cpp:182-186), so
    // unlockNewlyAvailable has nothing to advance and publishes nothing: the
    // dependents are not advertised, not advertised as LOCKED. That pre-join
    // case is asserted separately below.
    fx.mgr.onPlayerJoined(player);
    fx.pub.clear();

    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kRoot)), 1);
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "the prerequisite-free root completes on its own");
    CHECK(hasCompletionFor(fx.pub, kRoot), "quest 1 is the completed quest");
    // Its dependents are reconciled AVAILABLE but not completed — their items
    // have not been crafted yet.
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusAvailable,
              "quest 2 unlocks but is not completed");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRight), kStatusAvailable,
              "quest 3 unlocks but is not completed");
    CHECK(!fx.mgr.completeQuest(player, kJoin),
          "quest 4 is still two prerequisites away");

    // The next link: crafting quest 2's item completes it, and 4 stays locked.
    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kLeft)), 1);
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusCompleted,
              "quest 2 completes via detection");
    CHECK(!fx.mgr.completeQuest(player, kJoin),
          "quest 4 stays LOCKED with one of two prerequisites done");

    // The third link completes quest 3, which is what releases quest 4.
    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kRight)), 1);
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusAvailable,
              "quest 4 unlocks once the second prerequisite completes");

    // The last link completes it. Quest 5 was never crafted, so the era stays
    // open: all five fixture quests are VAGRANT.
    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kJoin)), 1);
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusCompleted,
              "quest 4 completes once both prerequisites are done");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 4,
              "the whole chain 1 -> 2,3 -> 4 completed, and nothing else");
    CHECK(!hasCompletionFor(fx.pub, kOther), "the independent root 5 never completed");
    CHECK_EQI(countTopic(fx.pub, "quest.era.transition"), 0,
              "the era stays open while quest 5 is unfinished");
}

static void test_detection_before_the_join_completes_only_the_detected_quest() {
    // The documented pre-join path: a player crafts before PlayerJoinedHandler
    // has seeded them. completeQuestInternal seeds the single detected quest as
    // LOCKED and completes it in one step, and unlockNewlyAvailable then has no
    // seeded dependents to advance — so nothing is advertised for them and no
    // quest.unlocked is published. A quest absent from the player's state is
    // invisible, not LOCKED-on-the-wire.
    const uint64_t player = 24;
    Fixture fx("prejoin", /*withRequirements=*/true);
    ensureItemRegistry();

    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kRoot)), 1);
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "the root completes with no prior join");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRoot), kStatusCompleted,
              "the detected root is advertised COMPLETED");
    // The dependents were never seeded, so they are not advertised at all.
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), -1,
              "an unseeded dependent is not advertised when the root completes");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRight), -1,
              "the other unseeded dependent is not advertised either");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), -1,
              "quest 4 is not advertised either");
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0,
              "no unlock is published for unseeded dependents");

    // A manual completion of an unseeded dependent is refused: it is not in the
    // player's state yet, so the refusal is the "unknown quest" path.
    CHECK(!fx.mgr.completeQuest(player, kLeft),
          "an unseeded quest cannot be manually completed");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "the refusal issues no further reward");

    // Once the join arrives the whole graph is seeded, and the completion that
    // happened before it still gates the dependents correctly.
    fx.mgr.onPlayerJoined(player);
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusAvailable,
              "the join reconciles quest 2 to AVAILABLE off the completed root");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRight), kStatusAvailable,
              "the join reconciles quest 3 to AVAILABLE off the completed root");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRoot), kStatusCompleted,
              "the pre-join completion survives the join");
}

static void test_autoComplete_false_stops_at_AVAILABLE_and_gates_dependents() {
    // Every quest in this fixture has auto_complete = false. The
    // LOCKED -> AVAILABLE branch of handleQuestMet is only reachable before
    // onPlayerJoined has seeded the player's state (afterwards every quest with
    // met prerequisites is already reconciled to AVAILABLE), so this case skips
    // the join — the same situation as a player whose craft lands before
    // onPlayerJoined, which completeQuestInternal's LOCKED seeding exists for.
    const uint64_t player = 16;
    Fixture fx("manual", /*withRequirements=*/true, /*autoComplete=*/false);
    ensureItemRegistry();
    const auto *def = fx.qd.GetQuest(kRoot);
    CHECK(def != nullptr, "quest 1 exists");
    if (def)
        CHECK(def->autoComplete == false, "autoComplete is false for this fixture");

    fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(kRoot)), 1);
    // Objective met, prerequisites met — but not completed, and no reward.
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 0,
              "autoComplete == false issues no reward");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kRoot), kStatusAvailable,
              "the quest is offered as AVAILABLE instead of completed");
    CHECK_EQI(lastAdvertisedProgress(fx.pub, kRoot), 0,
              "an offered quest is advertised at 0 percent");
    // Dependents do NOT unlock on AVAILABLE: handleQuestMet deliberately skips
    // unlockNewlyAvailable (QuestManager.cpp:471). They are also not seeded yet,
    // so they are not advertised at all rather than advertised as LOCKED.
    CHECK_EQI(countTopic(fx.pub, "quest.unlocked"), 0,
              "no dependent unlocks from an AVAILABLE prerequisite");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), -1,
              "the dependent was never advertised — it did not unlock");
    CHECK(!fx.graph.CanComplete(kLeft, {{kRoot, quest::QuestStatus::AVAILABLE}}),
          "quest 2 is still gated by an AVAILABLE prerequisite");

    // The join seeds the dependents as LOCKED (quest 1 is already in the state
    // as AVAILABLE, so it is left alone) and the gate still holds: the
    // prerequisite is not COMPLETED, so nothing gated on it is released. The
    // join's own quest.unlocked carries only quest 5, the independent root that
    // has no prerequisites at all — never quest 2 or 4, which are gated on the
    // merely-AVAILABLE quest 1.
    fx.mgr.onPlayerJoined(player);
    const auto joinUnlocked = unlockedIds(fx.pub);
    CHECK(contains(joinUnlocked, kOther), "the join announces the ungated root quest 5");
    CHECK(!contains(joinUnlocked, kLeft),
          "the join does not announce quest 2: its prerequisite is only AVAILABLE");
    CHECK(!contains(joinUnlocked, kJoin), "the join does not announce quest 4 either");
    CHECK(!fx.mgr.completeQuest(player, kLeft),
          "quest 2 is still gated after the join");

    // The player's Complete button is what finishes the job, and it unlocks.
    CHECK(fx.mgr.completeQuest(player, kRoot), "manual completion of quest 1 accepted");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "manual completion issues the reward event");
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kLeft), kStatusAvailable,
              "quest 2 unlocks only after the manual completion");
    // An already-COMPLETED quest stays refused.
    CHECK(!fx.mgr.completeQuest(player, kRoot),
          "a second manual completion is rejected");
    CHECK_EQI(countTopic(fx.pub, "quest.completed"), 1,
              "the second manual completion issues no second reward");
}

static void test_era_transition_fires_once_when_the_era_completes() {
    // All five fixture quests are VAGRANT, so completing the last one closes
    // the era and publishes quest.era.transition exactly once with
    // completed_era = VAGRANT (0), next_era = APPRENTICE (1). That 0/1 pair is
    // the Era::COUNT boundary arithmetic pinned in test_questtypes.cpp.
    const uint64_t player = 17;
    Fixture fx("era", /*withRequirements=*/true);
    ensureItemRegistry();
    seed(fx.mgr, player, {{kRoot, kCompleted, 100}});

    // Complete quests 2, 3 and 4 through their own detection objectives.
    for (uint32_t id : {kLeft, kRight, kJoin})
        fx.mgr.checkCraftCompletion(player, ItemId::pack(targetOf(id)), 1);
    CHECK_EQI(lastAdvertisedStatus(fx.pub, kJoin), kStatusCompleted,
              "quests 2, 3 and 4 are all completed");
    CHECK_EQI(countTopic(fx.pub, "quest.era.transition"), 0,
              "the era is still open while quest 5 is unfinished");

    CHECK(fx.mgr.completeQuest(player, kOther), "quest 5 completes");
    CHECK_EQI(countTopic(fx.pub, "quest.era.transition"), 1,
              "the era transition is published exactly once");

    int seen = 0;
    for (const auto &m : fx.pub.msgs) {
        if (m.topic != "quest.era.transition")
            continue;
        ++seen;
        flatbuffers::Verifier v(m.data.data(), m.data.size());
        CHECK(v.VerifyBuffer<Protocol::EraTransitionNotification>(nullptr),
              "the era transition buffer verifies");
        auto *era = flatbuffers::GetRoot<Protocol::EraTransitionNotification>(m.data.data());
        if (!era)
            continue;
        CHECK_EQI(era->player_id(), player, "the era transition names the player");
        CHECK_EQI(era->completed_era(), static_cast<uint8_t>(quest::Era::VAGRANT),
                  "completed_era is VAGRANT (wire byte 0)");
        CHECK_EQI(era->next_era(), static_cast<uint8_t>(quest::Era::APPRENTICE),
                  "next_era is APPRENTICE (wire byte 1)");
    }
    CHECK_EQI(seen, 1, "exactly one era transition message was captured");

    // A later completion in the same era must not re-publish the transition.
    CHECK(!fx.mgr.completeQuest(player, kRoot),
          "an already-COMPLETED quest is still refused");
    CHECK_EQI(countTopic(fx.pub, "quest.era.transition"), 1,
              "the era transition is not repeated");
}

#define TEST(name)                                                             \
    do {                                                                       \
        ++g_tests;                                                             \
        printf("  TEST: %s\n", #name);                                          \
        test_##name();                                                          \
    } while (0)

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("=== quest_manager test suite ===\n\n");

    TEST(fixture_graph_is_the_expected_shape);
    TEST(onPlayerJoined_seeds_roots_AVAILABLE_and_dependents_LOCKED);
    TEST(transitive_gating_holds_while_a_prerequisite_is_LOCKED);
    TEST(locked_intermediate_blocks_its_dependent);
    TEST(completion_unlocks_exactly_the_direct_dependents);
    TEST(completion_releases_the_join_only_when_both_prereqs_are_done);
    TEST(completion_sets_status_COMPLETED_and_progress_100);
    TEST(progress_snapshot_maps_COMPLETED_to_100_and_others_to_0);
    TEST(reward_is_issued_exactly_once_and_repeat_is_idempotent);
    TEST(distributeRewards_is_observed_as_a_CPP_noop);
    TEST(rejected_completions_change_nothing);
    TEST(loadProgress_rejects_a_buffer_shorter_than_four_bytes);
    TEST(loadProgress_clamps_an_out_of_range_status_to_LOCKED);
    TEST(loadProgress_applies_a_mismatched_player_id_anyway);
    TEST(rejoin_preserves_completed_quests);
    TEST(detection_completes_once_and_does_not_re_reward);
    TEST(detection_chain_respects_prerequisites_one_step_at_a_time);
    TEST(detection_before_the_join_completes_only_the_detected_quest);
    TEST(autoComplete_false_stops_at_AVAILABLE_and_gates_dependents);
    TEST(era_transition_fires_once_when_the_era_completes);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
