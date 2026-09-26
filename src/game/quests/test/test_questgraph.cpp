// QuestData graph construction + QuestGraph evaluation unit tests (gp-9l8f).
//
// src/game/quests/QuestData.cpp (404 lines) and QuestGraph.cpp (81 lines) are
// where a malformed quest file surfaces: every prerequisite edge the rest of
// the game trusts (QuestManager gating, unlock cascades, era completion) is
// built here. This test drives them with fully synthetic fixtures — a temp
// quests.csv and a temp quest_graph.json per case — so the real
// src/content/data/quests/ dataset is never touched and each case states its
// own graph.
//
// What is pinned:
//   1. LoadCSV starts with an empty reverse index (the 9-column CSV carries no
//      prerequisites at all — they moved to quest_graph.json),
//   2. LoadGraph builds one reverse edge per declared requirement,
//   3. LoadGraph rewrites the QuestDef prerequisite list in place, so
//      quest_graph.json is the source of truth for every consumer,
//   4. malformed graph files fail (missing / unparseable / wrong root type) and
//      leave the previously loaded graph intact,
//   5. a JSON quest absent from the CSV is skipped, not half-inserted,
//   6. CanComplete / LockedByPrereqs / NewlyAvailable / GetUnlocked agree with
//      each other, and unknown quest ids are inert,
//   7. a prerequisite cycle is NOT detected: QuestGraph never recurses, so it
//      cannot loop forever, but a cyclic quest can never be completed — the
//      observed contract, asserted as observed (see the comment on
//      test_QuestGraph_cycle_does_not_recurse_but_deadlocks),
//   8. a dangling prerequisite (an id no quest declares) and a self-dependency
//      are both retained and both block forever — also observed behaviour,
//   9. BuildQuestEraMap drops EXCHANGE quests (repeatable market quests never
//      complete, so counting them would make an era permanently uncompletable),
//      and IsEraComplete skips ids absent from the era map.
//
// The fixtures are deterministic (no clock, no randomness, no network) and the
// order-insensitive queries (NewlyAvailable/GetUnlocked walk an unordered_map)
// are sorted before comparison, so no case can flake.
#include <game/quests/QuestData.h>
#include <game/quests/QuestGraph.h>
#include <engine/net/test/test.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

namespace fs = std::filesystem;

namespace {

// A temp file that deletes itself, so each case owns its name and a failed case
// leaves nothing behind.
struct TempFile {
    fs::path path;
    TempFile() = default;
    explicit TempFile(const std::string &name)
        : path(fs::temp_directory_path() / ("quest_graph_test_" + name)) {}
    void write(const std::string &contents) const {
        std::ofstream of(path);
        of << contents;
    }
    std::string str() const { return path.string(); }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

// quests.csv row: id,title,description,era,section,cost_item,cost_count,
// cooldown,target_count — note there is NO prerequisite column left.
std::string csvRow(uint32_t id, const std::string &era = "vagrant",
                   const std::string &section = "misc") {
    return std::to_string(id) + ",Quest " + std::to_string(id) +
           ",Synthetic quest," + era + "," + section + ",,,\n";
}

TempFile makeCsv(const std::string &name, const std::string &rows) {
    TempFile f(name);
    f.write("id,title,description,era,section,cost_item,cost_count,cooldown,"
            "target_count\n" +
            rows);
    return f;
}

// quest_graph.json: {"quests": [{"id": N, "prereqs": [...]}, ...]}
std::string graphEntry(uint32_t id, const std::vector<uint32_t> &prereqs) {
    std::string s = "{\"id\": " + std::to_string(id) + ", \"prereqs\": [";
    for (size_t i = 0; i < prereqs.size(); ++i) {
        if (i)
            s += ", ";
        s += std::to_string(prereqs[i]);
    }
    return s + "]}";
}

std::string graphJson(const std::string &entries) {
    return "{\"quests\": [" + entries + "]}";
}

TempFile makeGraph(const std::string &name, const std::string &entries) {
    TempFile f(name);
    f.write(graphJson(entries));
    return f;
}

// Mirrors production main.cpp:531-536 — the prereq map every caller must build
// before QuestGraph::Init, derived from the QuestDefs' prerequisite lists.
void initGraph(quest::QuestData &qd, quest::QuestGraph &graph) {
    std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
    for (const auto &q : qd.AllQuests())
        prereqs[q.id] = q.prerequisites;
    graph.Init(qd.Graph(), prereqs);
}

std::vector<uint32_t> sorted(std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

bool contains(const std::vector<uint32_t> &v, uint32_t id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

using Progress = std::unordered_map<uint32_t, quest::QuestStatus>;

} // namespace

// ---------------------------------------------------------------------------
// 1. LoadCSV leaves the reverse index empty
// ---------------------------------------------------------------------------
static void test_QuestData_LoadCSV_starts_with_an_empty_reverse_index() {
    TempFile csv = makeCsv("csv_empty", csvRow(1) + csvRow(2) + csvRow(3));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV on a 3-quest fixture");
    CHECK_EQI(qd.Count(), 3, "all three rows parsed");
    // quests.csv is the 9-column schema (QuestData.cpp:37-63) and no column
    // declares prerequisites, so buildGraph() has nothing to index.
    CHECK(qd.Graph().empty(),
          "LoadCSV alone declares no prerequisites -> no graph edges");
    CHECK_EQI(qd.GetRootQuests().size(), 3, "every quest is a root until LoadGraph");
    CHECK(qd.GetChildren(1).empty(), "no children before LoadGraph");
    CHECK(!qd.LoadCSV("/nonexistent/quests.csv"),
          "a missing CSV fails instead of throwing");
}

static void test_QuestData_LoadGraph_builds_one_edge_per_requirement() {
    TempFile csv = makeCsv("edges", csvRow(1) + csvRow(2) + csvRow(3) + csvRow(4));
    // 1 is a root; 2 and 3 depend on 1; 4 depends on both 2 and 3.
    TempFile graph = makeGraph("json_edges", graphEntry(1, {}) + ", " +
                                                    graphEntry(2, {1}) + ", " +
                                                    graphEntry(3, {1}) + ", " +
                                                    graphEntry(4, {2, 3}));

    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");

    // The reverse index is keyed prereq -> dependents (QuestData.cpp:139-141).
    CHECK_EQI(qd.GetChildren(1).size(), 2, "quest 1 has two dependents (2 and 3)");
    CHECK(contains(qd.GetChildren(1), 2), "1 -> 2");
    CHECK(contains(qd.GetChildren(1), 3), "1 -> 3");
    CHECK(contains(qd.GetChildren(2), 4), "2 -> 4");
    CHECK(contains(qd.GetChildren(3), 4), "3 -> 4");
    CHECK(qd.GetChildren(4).empty(), "leaf quest 4 has no children");
    CHECK_EQI(qd.Graph().size(), 3, "three nodes carry outgoing edges (1, 2, 3)");

    // GetPrerequisites reads back the same declaration order as the JSON.
    CHECK_EQI(qd.GetPrerequisites(4).size(), 2, "quest 4 declares two prerequisites");
    CHECK_EQI(qd.GetPrerequisites(4)[0], 2, "quest 4's first prerequisite is 2");
    CHECK_EQI(qd.GetPrerequisites(4)[1], 3, "quest 4's second prerequisite is 3");
    CHECK(qd.GetPrerequisites(1).empty(), "root quest 1 declares no prerequisites");

    // Roots are derived from the (JSON-sourced) prerequisite lists.
    const auto roots = sorted(qd.GetRootQuests());
    CHECK_EQI(roots.size(), 1, "exactly one root in this fixture");
    CHECK_EQI(roots[0], 1, "quest 1 is the only root");

    // Unknown ids are inert everywhere, not errors.
    CHECK(qd.GetChildren(9999).empty(), "an undeclared quest id has no children");
    CHECK(qd.GetQuest(9999) == nullptr, "an undeclared quest id has no QuestDef");
    CHECK(qd.GetPrerequisites(9999).empty(),
          "GetPrerequisites on an unknown id returns the shared empty vector");
}

// ---------------------------------------------------------------------------
// 2. quest_graph.json is the source of truth for prerequisites
// ---------------------------------------------------------------------------
static void test_QuestData_LoadGraph_overwrites_prereqs_in_place() {
    TempFile csv =
        makeCsv("override", csvRow(1) + csvRow(2) + csvRow(11) + csvRow(12));
    TempFile graph = makeGraph("json_override", graphEntry(1, {}) + ", " +
                                                      graphEntry(2, {1}) + ", " +
                                                      graphEntry(11, {}) + ", " +
                                                      graphEntry(12, {11}));

    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");

    // The CSV declared no prerequisites at all, so every edge here came from
    // the JSON — and QuestData stored the JSON list on the QuestDef itself
    // (QuestData.cpp:137), not only in the reverse index.
    const auto *q12 = qd.GetQuest(12);
    CHECK(q12 != nullptr, "quest 12 exists");
    if (q12) {
        CHECK_EQI(q12->prerequisites.size(), 1, "quest 12 carries one prerequisite");
        CHECK_EQI(q12->prerequisites[0], 11, "quest 12 depends on 11");
    }
    const auto *q2 = qd.GetQuest(2);
    CHECK(q2 != nullptr, "quest 2 exists");
    if (q2) {
        CHECK_EQI(q2->prerequisites.size(), 1, "quest 2 carries one prerequisite");
        CHECK_EQI(q2->prerequisites[0], 1, "quest 2 depends on 1");
    }
    // A quest the JSON does not mention keeps the CSV value (empty here) and
    // gains no edges.
    const auto *q11 = qd.GetQuest(11);
    CHECK(q11 != nullptr, "quest 11 exists");
    if (q11)
        CHECK(q11->prerequisites.empty(), "quest 11 is a root");

    CHECK(contains(qd.GetChildren(11), 12), "11 -> 12 edge present");
    CHECK(contains(qd.GetChildren(1), 2), "1 -> 2 edge present");
    CHECK(qd.GetChildren(2).empty(), "quest 2 has no dependents in this fixture");
    CHECK(qd.GetChildren(12).empty(), "quest 12 has no dependents in this fixture");
}

static void test_QuestData_LoadGraph_rejects_malformed_input() {
    quest::QuestData never;
    CHECK(!never.LoadGraph("/nonexistent/quest_graph.json"),
          "a missing file fails instead of throwing");

    TempFile csv = makeCsv("malformed", csvRow(1) + csvRow(2));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");

    TempFile good = makeGraph("json_malformed_good",
                              graphEntry(1, {}) + ", " + graphEntry(2, {1}));
    CHECK(qd.LoadGraph(good.str()), "a valid graph loads");
    CHECK_EQI(qd.GetPrerequisites(2).size(), 1, "quest 2 depends on 1 after load");
    const auto childrenAfterGood = qd.GetChildren(1);
    CHECK_EQI(childrenAfterGood.size(), 1, "1 -> 2 edge built by the valid load");

    TempFile notJson("json_malformed_text");
    notJson.write("this is not json");
    CHECK(!qd.LoadGraph(notJson.str()), "unparseable JSON fails");

    TempFile wrongRoot("json_malformed_root");
    wrongRoot.write("{\"nodes\": []}");
    CHECK(!qd.LoadGraph(wrongRoot.str()), "a JSON object without 'quests' fails");

    TempFile wrongType("json_malformed_type");
    wrongType.write("{\"quests\": {\"1\": {}}}");
    CHECK(!qd.LoadGraph(wrongType.str()), "a non-array 'quests' fails");

    // graph_.swap() only runs at the end of a successful parse
    // (QuestData.cpp:145), so three failed loads must leave the graph and the
    // per-quest prerequisite lists exactly as the valid load left them.
    CHECK_EQI(qd.GetPrerequisites(2).size(), 1,
              "a failed LoadGraph does not clear the stored prerequisites");
    CHECK_EQI(qd.GetPrerequisites(2)[0], 1,
              "quest 2 still depends on 1 after three failed loads");
    const auto childrenNow = qd.GetChildren(1);
    CHECK_EQI(childrenNow.size(), 1, "the 1 -> 2 edge survived the failed loads");
    CHECK(contains(childrenNow, 2), "the surviving edge is still 1 -> 2");
}

static void test_QuestData_LoadGraph_skips_a_quest_missing_from_the_csv() {
    // QuestData.cpp:108-111 — a JSON quest that is not in the CSV is warned
    // about and skipped, not inserted. Assert the *effect*: the quest does not
    // exist and no QuestDef ends up depending on it.
    TempFile csv = makeCsv("missing_node", csvRow(1) + csvRow(2) + csvRow(3));
    TempFile graph = makeGraph("json_missing_node",
                              graphEntry(1, {}) + ", " + graphEntry(2, {1}) +
                                  ", " + graphEntry(777, {1}) + ", " +
                                  graphEntry(3, {2}));

    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph still succeeds");
    CHECK_EQI(qd.Count(), 3, "the CSV quest count is unchanged by the skipped node");

    CHECK(qd.GetQuest(777) == nullptr,
          "the CSV-absent quest 777 was not inserted into the QuestDef table");
    CHECK(qd.GetPrerequisites(777).empty(),
          "no prerequisite row for a skipped quest");
    for (const auto &def : qd.AllQuests()) {
        for (uint32_t p : def.prerequisites)
            CHECK(p != 777, "no stored QuestDef references the skipped quest 777");
    }
    // 777's own outgoing edge (777 is a leaf) must not exist either.
    CHECK(qd.GetChildren(777).empty(), "the skipped quest has no reverse edges");
    // Quest 3 (prereq 2) is unaffected by the skipped node.
    CHECK(contains(qd.GetChildren(2), 3), "2 -> 3 edge still built");
    CHECK(contains(qd.GetChildren(1), 2), "1 -> 2 edge still built");
}

// ---------------------------------------------------------------------------
// 3. QuestGraph evaluation
// ---------------------------------------------------------------------------
static void test_QuestGraph_CanComplete_requires_every_prerequisite() {
    // 1 root; 2 and 3 depend on 1; 4 depends on both 2 and 3.
    TempFile csv = makeCsv("eval", csvRow(1) + csvRow(2) + csvRow(3) + csvRow(4));
    TempFile graph = makeGraph("json_eval", graphEntry(1, {}) + ", " +
                                                  graphEntry(2, {1}) + ", " +
                                                  graphEntry(3, {1}) + ", " +
                                                  graphEntry(4, {2, 3}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");
    quest::QuestGraph qg;
    initGraph(qd, qg);

    // No prerequisites declared -> completable, even against an empty map.
    CHECK(qg.CanComplete(1, {}), "quest 1 has no prerequisites");
    // A prerequisite absent from `current` counts as NOT completed
    // (QuestGraph.cpp:20) — "unknown is not done", not "unknown is allowed".
    CHECK(!qg.CanComplete(2, {}), "quest 2 with no recorded progress cannot complete");

    const Progress only1{{1, quest::QuestStatus::COMPLETED}};
    CHECK(qg.CanComplete(2, only1), "quest 2 unlocks when 1 is COMPLETED");
    CHECK(qg.CanComplete(3, only1), "quest 3 unlocks when 1 is COMPLETED");
    CHECK(!qg.CanComplete(4, only1), "quest 4 still needs 2 and 3");

    const Progress both{{1, quest::QuestStatus::COMPLETED},
                        {2, quest::QuestStatus::COMPLETED},
                        {3, quest::QuestStatus::COMPLETED}};
    CHECK(qg.CanComplete(4, both), "quest 4 unlocks when 2 and 3 are COMPLETED");

    // A prereq in any non-COMPLETED status blocks, including one that is
    // itself AVAILABLE or mid-progress.
    Progress partial{{1, quest::QuestStatus::COMPLETED},
                     {2, quest::QuestStatus::AVAILABLE},
                     {3, quest::QuestStatus::COMPLETED}};
    CHECK(!qg.CanComplete(4, partial), "AVAILABLE prereq 2 does not satisfy quest 4");
    partial[2] = quest::QuestStatus::IN_PROGRESS;
    CHECK(!qg.CanComplete(4, partial), "IN_PROGRESS prereq 2 does not satisfy quest 4");
    partial[2] = quest::QuestStatus::LOCKED;
    CHECK(!qg.CanComplete(4, partial), "LOCKED prereq 2 does not satisfy quest 4");
    partial[2] = quest::QuestStatus::COMPLETED;
    CHECK(qg.CanComplete(4, partial), "COMPLETED prereq 2 satisfies quest 4");
    // A quest's own status is irrelevant to CanComplete — only its
    // prerequisites' statuses are read.
    CHECK(qg.CanComplete(1, {{1, quest::QuestStatus::LOCKED}}),
          "CanComplete reads prerequisites, not the quest's own status");

    // An id that is not in the graph at all is treated as a prerequisite-free
    // quest rather than an error.
    CHECK(qg.CanComplete(9999, both), "an unknown quest id has no prerequisites");
}

static void test_QuestGraph_LockedByPrereqs_lists_only_the_unmet() {
    TempFile csv = makeCsv("locked_by", csvRow(1) + csvRow(2) + csvRow(3) + csvRow(4));
    TempFile graph = makeGraph("json_locked_by", graphEntry(1, {}) + ", " +
                                                      graphEntry(2, {1}) + ", " +
                                                      graphEntry(3, {1}) + ", " +
                                                      graphEntry(4, {2, 3}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");
    quest::QuestGraph qg;
    initGraph(qd, qg);

    // Nothing recorded: both prerequisites block, in declaration order.
    const auto none = qg.LockedByPrereqs(4, {});
    CHECK_EQI(none.size(), 2, "quest 4 is blocked by 2 and 3");
    CHECK_EQI(none[0], 2, "first blocker is 2");
    CHECK_EQI(none[1], 3, "second blocker is 3");

    // One met: only the unmet one is reported, and LockedByPrereqs agrees with
    // CanComplete on the same state.
    const Progress one{{1, quest::QuestStatus::COMPLETED},
                       {2, quest::QuestStatus::COMPLETED}};
    const auto still = qg.LockedByPrereqs(4, one);
    CHECK_EQI(still.size(), 1, "only the unmet prerequisite 3 is reported");
    CHECK_EQI(still[0], 3, "the single blocker is 3");
    CHECK(!qg.CanComplete(4, one), "CanComplete agrees: quest 4 is still blocked");

    CHECK(qg.LockedByPrereqs(1, {}).empty(),
          "a prerequisite-free quest has no blockers");
    CHECK(qg.LockedByPrereqs(9999, {}).empty(),
          "an unknown quest id has no blockers");
}

static void test_QuestGraph_NewlyAvailable_only_advances_LOCKED_quests() {
    TempFile csv = makeCsv("newly", csvRow(1) + csvRow(2) + csvRow(3) + csvRow(4));
    TempFile graph = makeGraph("json_newly", graphEntry(1, {}) + ", " +
                                                  graphEntry(2, {1}) + ", " +
                                                  graphEntry(3, {1}) + ", " +
                                                  graphEntry(4, {2, 3}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");
    quest::QuestGraph qg;
    initGraph(qd, qg);

    // Every quest still LOCKED: only the prerequisite-free root unlocks.
    const Progress locked{{1, quest::QuestStatus::LOCKED},
                           {2, quest::QuestStatus::LOCKED},
                           {3, quest::QuestStatus::LOCKED},
                           {4, quest::QuestStatus::LOCKED}};
    const auto initial = sorted(qg.NewlyAvailable(locked));
    CHECK_EQI(initial.size(), 1, "only the prerequisite-free root unlocks first");
    CHECK_EQI(initial[0], 1, "quest 1 is newly available");

    // Completing 1 unlocks 2 and 3 in one pass, but not 4 (needs 2 AND 3).
    const Progress after1{{1, quest::QuestStatus::COMPLETED},
                          {2, quest::QuestStatus::LOCKED},
                          {3, quest::QuestStatus::LOCKED},
                          {4, quest::QuestStatus::LOCKED}};
    const auto second = sorted(qg.NewlyAvailable(after1));
    CHECK_EQI(second.size(), 2, "completing quest 1 unlocks 2 and 3");
    CHECK_EQI(second[0], 2, "2 is newly available");
    CHECK_EQI(second[1], 3, "3 is newly available");
    CHECK(!contains(second, 4), "quest 4 needs both 2 and 3 COMPLETED");

    // Only 2 completed: 3 is the only LOCKED quest with its prereq met.
    const Progress after2{{1, quest::QuestStatus::COMPLETED},
                          {2, quest::QuestStatus::COMPLETED},
                          {3, quest::QuestStatus::LOCKED},
                          {4, quest::QuestStatus::LOCKED}};
    const auto third = qg.NewlyAvailable(after2);
    CHECK_EQI(third.size(), 1, "only quest 3 unlocks next");
    CHECK_EQI(third[0], 3, "3 is the next unlock");

    // A quest that is no longer LOCKED is never re-reported, and quest 4 is
    // still gated: quest 2 is only AVAILABLE, not COMPLETED.
    const Progress mixed{{1, quest::QuestStatus::COMPLETED},
                         {2, quest::QuestStatus::AVAILABLE},
                         {3, quest::QuestStatus::LOCKED},
                         {4, quest::QuestStatus::LOCKED}};
    const auto fourth = sorted(qg.NewlyAvailable(mixed));
    CHECK_EQI(fourth.size(), 1, "AVAILABLE / COMPLETED quests are not re-reported");
    CHECK_EQI(fourth[0], 3, "3 is still newly available");
    CHECK(!contains(fourth, 4), "4 stays locked: its prereq 2 is only AVAILABLE");

    // A state with nothing LOCKED yields nothing.
    CHECK(qg.NewlyAvailable({{1, quest::QuestStatus::COMPLETED}}).empty(),
          "no LOCKED quest means nothing newly available");
    CHECK(qg.NewlyAvailable({{1, quest::QuestStatus::AVAILABLE},
                             {2, quest::QuestStatus::COMPLETED}})
              .empty(),
          "AVAILABLE quests are not re-reported as newly available");
}

static void test_QuestGraph_GetUnlocked_reports_LOCKED_but_not_AVAILABLE() {
    // 1 root; 2 depends on 1; 3 depends on 2.
    TempFile csv = makeCsv("unlocked", csvRow(1) + csvRow(2) + csvRow(3));
    TempFile graph = makeGraph("json_unlocked", graphEntry(1, {}) + ", " +
                                                      graphEntry(2, {1}) + ", " +
                                                      graphEntry(3, {2}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");
    quest::QuestGraph qg;
    initGraph(qd, qg);

    // 1 COMPLETED, 2 and 3 LOCKED: only 2 is a fresh unlock.
    const Progress first{{1, quest::QuestStatus::COMPLETED},
                         {2, quest::QuestStatus::LOCKED},
                         {3, quest::QuestStatus::LOCKED}};
    const auto got = sorted(qg.GetUnlocked(first));
    CHECK_EQI(got.size(), 1, "only the LOCKED quest 2 is reported as unlocked");
    CHECK_EQI(got[0], 2, "quest 2 is the unlock");

    // 2 moves to AVAILABLE: it drops out of the result, and 3 stays out because
    // its prerequisite must be COMPLETED, not merely AVAILABLE
    // (QuestGraph.cpp:43-47 pushes only LOCKED quests that CanComplete).
    const Progress second{{1, quest::QuestStatus::COMPLETED},
                          {2, quest::QuestStatus::AVAILABLE},
                          {3, quest::QuestStatus::LOCKED}};
    const auto got2 = qg.GetUnlocked(second);
    CHECK(got2.empty(), "an AVAILABLE prerequisite does not unlock the dependent");
    CHECK(!contains(got2, 2), "the AVAILABLE quest is no longer reported");
    CHECK(!contains(got2, 3), "quest 3 stays locked while 2 is only AVAILABLE");

    // 2 COMPLETED: now 3 unlocks, and 2 is not re-reported.
    const Progress third{{1, quest::QuestStatus::COMPLETED},
                         {2, quest::QuestStatus::COMPLETED},
                         {3, quest::QuestStatus::LOCKED}};
    const auto got3 = qg.GetUnlocked(third);
    CHECK_EQI(got3.size(), 1, "quest 3 is the only new unlock");
    CHECK_EQI(got3[0], 3, "quest 3 unlocks once quest 2 is COMPLETED");

    // A prerequisite-free quest that is still LOCKED is reported (it was never
    // marked AVAILABLE), so a client can pick it up.
    const Progress fourth{{1, quest::QuestStatus::LOCKED}};
    const auto got4 = qg.GetUnlocked(fourth);
    CHECK_EQI(got4.size(), 1, "a LOCKED root quest is reported as unlocked");
    CHECK_EQI(got4[0], 1, "the reported unlock is quest 1");
}

// ---------------------------------------------------------------------------
// 4. Cycles, self-dependencies and dangling prerequisites — observed behaviour
// ---------------------------------------------------------------------------
static void test_QuestGraph_cycle_is_never_detected_and_never_recurses() {
    // 1 <-> 2, and 3 depends on both. QuestGraph walks the prerequisite list
    // non-recursively (CanComplete is a flat loop over prereqs_ that only reads
    // the *direct* prereq's current status), so:
    //   * a cycle cannot cause unbounded recursion — every query terminates;
    //   * a cycle is never detected or reported at load time either;
    //   * and because there is no transitive analysis, a cycle member becomes
    //     completable as soon as its DIRECT prereq is marked COMPLETED. The
    //     graph does not notice that the prereq is itself only reachable
    //     through this quest.
    // That is the contract as written. Asserted as observed so a future cycle
    // check or transitive re-evaluation surfaces as a deliberate test change.
    TempFile csv = makeCsv("cycle", csvRow(1) + csvRow(2) + csvRow(3));
    TempFile graph = makeGraph("json_cycle", graphEntry(1, {2}) + ", " +
                                                  graphEntry(2, {1}) + ", " +
                                                  graphEntry(3, {1, 2}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph: a cycle loads without error");

    // The cycle edges are kept verbatim, not collapsed, and not reported.
    CHECK_EQI(qd.GetPrerequisites(1).size(), 1, "quest 1 keeps its prerequisite 2");
    CHECK_EQI(qd.GetPrerequisites(1)[0], 2, "quest 1 depends on 2");
    CHECK_EQI(qd.GetPrerequisites(2).size(), 1, "quest 2 keeps its prerequisite 1");
    CHECK_EQI(qd.GetPrerequisites(2)[0], 1, "quest 2 depends on 1");
    CHECK(contains(qd.GetChildren(1), 2), "1 -> 2 edge kept");
    CHECK(contains(qd.GetChildren(2), 1), "2 -> 1 edge kept");
    // A pure cycle has no root, so a fresh player is never offered an entry
    // point by this graph.
    CHECK(qd.GetRootQuests().empty(), "a pure cycle has no root quest");

    quest::QuestGraph qg;
    initGraph(qd, qg);

    // The exact answer, quest by quest: CanComplete is "every DIRECT prereq is
    // COMPLETED", nothing more. A prerequisite that is itself blocked does not
    // block its dependent a second time.
    const Progress empty;
    CHECK(!qg.CanComplete(1, empty), "quest 1 needs 2 COMPLETED");
    CHECK(!qg.CanComplete(2, empty), "quest 2 needs 1 COMPLETED");
    CHECK(!qg.CanComplete(3, empty), "quest 3 needs 1 and 2 COMPLETED");

    const Progress oneDone{{1, quest::QuestStatus::COMPLETED}};
    CHECK(!qg.CanComplete(1, oneDone), "quest 1 still needs 2 COMPLETED");
    // 2's prereq (1) is COMPLETED, so 2 is completable even though completing
    // 2 would close the loop back through 1. The graph does not look ahead.
    CHECK(qg.CanComplete(2, oneDone),
          "quest 2 is completable because its DIRECT prereq 1 is COMPLETED");
    CHECK(!qg.CanComplete(3, oneDone), "quest 3 still needs 2 COMPLETED");

    const Progress bothDone{{1, quest::QuestStatus::COMPLETED},
                            {2, quest::QuestStatus::COMPLETED}};
    CHECK(qg.CanComplete(1, bothDone), "quest 1 completes once 2 is COMPLETED");
    CHECK(qg.CanComplete(2, bothDone), "quest 2 completes once 1 is COMPLETED");
    CHECK(qg.CanComplete(3, bothDone), "quest 3 completes once 1 and 2 are COMPLETED");

    // Blockers follow the same flat rule: only unmet DIRECT prerequisites.
    CHECK_EQI(qg.LockedByPrereqs(1, empty).size(), 1, "quest 1 reports blocker 2");
    CHECK_EQI(qg.LockedByPrereqs(1, oneDone).size(), 1,
              "quest 1 still reports blocker 2 while 2 is not COMPLETED");
    CHECK(qg.LockedByPrereqs(1, bothDone).empty(),
          "quest 1 reports no blocker once 2 is COMPLETED");
    CHECK_EQI(qg.LockedByPrereqs(3, oneDone).size(), 1,
              "quest 3 reports only the still-unmet prerequisite 2");
    CHECK_EQI(qg.LockedByPrereqs(3, bothDone).size(), 0,
              "quest 3 reports no blockers once both prerequisites are COMPLETED");
    // NewlyAvailable terminates too: a cycle simply has no root to start from,
    // so an all-LOCKED state yields nothing.
    const std::unordered_map<uint32_t, quest::QuestStatus> allLocked{
        {1, quest::QuestStatus::LOCKED}, {2, quest::QuestStatus::LOCKED},
        {3, quest::QuestStatus::LOCKED}};
    CHECK_EQI(qg.NewlyAvailable(allLocked).size(), 0,
              "a pure cycle has no entry point for a fresh player");
}

static void test_QuestData_self_dependency_is_kept_as_an_edge() {
    // The stale quests.csv shape this fixture reproduces: a quest listing
    // itself as its own prerequisite. quest_graph.json is the source of truth
    // precisely because the shipped quests 12/13 declared that way.
    TempFile csv = makeCsv("self_dep", csvRow(13));
    TempFile graph = makeGraph("json_self_dep", graphEntry(13, {13}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");
    quest::QuestGraph qg;
    initGraph(qd, qg);

    CHECK_EQI(qd.GetPrerequisites(13).size(), 1, "the self-dependency is stored");
    CHECK_EQI(qd.GetPrerequisites(13)[0], 13, "quest 13 depends on itself");
    CHECK(contains(qd.GetChildren(13), 13), "13 -> 13 self-edge is built");
    CHECK(qd.GetRootQuests().empty(), "a self-dependent quest is not a root");
    // Never reported, never recursed — but a self-dependent quest is blocked
    // only until it is itself COMPLETED: the flat direct check reads its own
    // status, so a player whose MetaDB state already records quest 13 as
    // COMPLETED is not gated by it.
    CHECK(!qg.CanComplete(13, {}), "self-dependency blocks its own quest");
    CHECK(!qg.CanComplete(13, {{13, quest::QuestStatus::LOCKED}}),
          "a LOCKED self-dependent quest stays blocked");
    CHECK(!qg.CanComplete(13, {{13, quest::QuestStatus::AVAILABLE}}),
          "an AVAILABLE self-dependent quest stays blocked");
    CHECK(qg.CanComplete(13, {{13, quest::QuestStatus::COMPLETED}}),
          "a COMPLETED self-dependent quest is not gated by itself");
    const auto blockers = qg.LockedByPrereqs(13, {{13, quest::QuestStatus::LOCKED}});
    CHECK_EQI(blockers.size(), 1, "the quest reports one blocker");
    CHECK_EQI(blockers[0], 13, "the quest reports itself as its own blocker");
    // And a self-dependent quest is never offered as newly available: it is not
    // a root, so nothing seeds it AVAILABLE on join.
    CHECK_EQI(qg.NewlyAvailable({{13, quest::QuestStatus::LOCKED}}).size(), 0,
              "a self-dependent quest is never newly available");
}

static void test_QuestData_dangling_prerequisite_is_kept_and_blocks_forever() {
    // A prerequisite naming a quest that exists nowhere: the edge is NOT
    // dropped, so the dependent is gated by an id that can never appear in a
    // player's progress. Asserted as observed — the requirement is silently
    // retained, which is why quest_graph.json must only reference declared
    // quests.
    TempFile csv = makeCsv("dangling", csvRow(1) + csvRow(2));
    TempFile graph =
        makeGraph("json_dangling", graphEntry(1, {}) + ", " + graphEntry(2, {1, 9999}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph reports success");

    CHECK_EQI(qd.GetPrerequisites(2).size(), 2, "the dangling id is retained");
    CHECK_EQI(qd.GetPrerequisites(2)[1], 9999, "the dangling prerequisite is 9999");
    CHECK(qd.GetQuest(9999) == nullptr, "9999 is not a declared quest");
    // 9999 becomes a graph node with an outgoing edge even though it is not a
    // quest — the reverse index is keyed on ids, not on QuestDefs.
    CHECK(contains(qd.GetChildren(9999), 2), "9999 -> 2 edge is still built");
    CHECK_EQI(qd.GetRootQuests().size(), 1, "quest 1 is the only root");

    quest::QuestGraph qg;
    initGraph(qd, qg);

    // A player who completed quest 1 is still blocked: 9999 is absent from every
    // progress map, and "absent" is treated as "not completed".
    const Progress after1{{1, quest::QuestStatus::COMPLETED},
                          {2, quest::QuestStatus::LOCKED}};
    CHECK(!qg.CanComplete(2, after1), "a dangling prerequisite blocks forever");
    const auto blockers = qg.LockedByPrereqs(2, after1);
    CHECK_EQI(blockers.size(), 1, "the dangling prerequisite is the reported blocker");
    CHECK_EQI(blockers[0], 9999, "the blocker is the dangling id, not a real quest");
    CHECK_EQI(qg.NewlyAvailable(after1).size(), 0,
              "a dependent of a dangling prerequisite never becomes available");
    // The one thing that would satisfy it is a progress entry for 9999 itself.
    // QuestManager never records one (it only stores statuses for quests it
    // knows, QuestManager.cpp:634-642), so in practice the gate is permanent —
    // but the graph's rule is the plain one, asserted here as written.
    CHECK(qg.CanComplete(2, {{1, quest::QuestStatus::COMPLETED},
                             {9999, quest::QuestStatus::COMPLETED}}),
          "a status recorded for the dangling id DOES unlock the dependent");
}

// ---------------------------------------------------------------------------
// 5. Era completion
// ---------------------------------------------------------------------------
static void test_QuestGraph_IsEraComplete_ignores_other_eras_and_unknowns() {
    TempFile csv = makeCsv("era", csvRow(1, "vagrant") + csvRow(2, "vagrant") +
                                   csvRow(3, "apprentice"));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    quest::QuestGraph qg;
    initGraph(qd, qg);

    const auto eraMap = qd.BuildQuestEraMap();
    CHECK_EQI(eraMap.size(), 3, "BuildQuestEraMap covers all three fixture quests");
    CHECK(eraMap.at(1) == quest::Era::VAGRANT, "quest 1 is VAGRANT");
    CHECK(eraMap.at(2) == quest::Era::VAGRANT, "quest 2 is VAGRANT");
    CHECK(eraMap.at(3) == quest::Era::APPRENTICE, "quest 3 is APPRENTICE");

    const Progress p{{1, quest::QuestStatus::COMPLETED},
                     {2, quest::QuestStatus::COMPLETED},
                     {3, quest::QuestStatus::LOCKED}};
    CHECK(qg.IsEraComplete(quest::Era::VAGRANT, p, eraMap),
          "VAGRANT is complete when both of its quests are COMPLETED");
    CHECK(!qg.IsEraComplete(quest::Era::APPRENTICE, p, eraMap),
          "APPRENTICE is not complete while quest 3 is LOCKED");
    // A quest in another era never affects this one.
    Progress vagrantLate{{1, quest::QuestStatus::COMPLETED},
                         {2, quest::QuestStatus::COMPLETED},
                         {3, quest::QuestStatus::COMPLETED}};
    CHECK(qg.IsEraComplete(quest::Era::VAGRANT, vagrantLate, eraMap),
          "completing another era's quest does not gate VAGRANT");
    // An era with no quest in the era map is vacuously complete: the loop skips
    // ids absent from questEraMap and nothing is left to fail on.
    CHECK(qg.IsEraComplete(quest::Era::EXPERT, p, eraMap),
          "an era with no quests is vacuously complete");
    // A quest present in progress but missing from the era map is skipped too
    // (QuestGraph.cpp:58-59), so it can never block an era.
    const Progress withStranger{{1, quest::QuestStatus::COMPLETED},
                                {2, quest::QuestStatus::COMPLETED},
                                {3, quest::QuestStatus::LOCKED},
                                {9999, quest::QuestStatus::LOCKED}};
    CHECK(qg.IsEraComplete(quest::Era::VAGRANT, withStranger, eraMap),
          "an id absent from the era map does not gate its era");
    // An empty era map makes every era vacuously complete.
    CHECK(qg.IsEraComplete(quest::Era::VAGRANT, p, {}),
          "an empty era map reports every era complete");
}

static void test_QuestData_BuildQuestEraMap_excludes_EXCHANGE_quests() {
    // QuestData.cpp:397-399 skips EXCHANGE quests: they are repeatable market
    // quests that never complete, so counting them would make an era
    // permanently uncompletable. Driven through a requirements fixture, since
    // the CSV cannot set detectType.
    TempFile csv = makeCsv("era_exchange", csvRow(1) + csvRow(2) + csvRow(3));
    TempFile reqs("json_era_exchange");
    reqs.write("{\n"
               "  \"2\": {\"requirements\": [{\"kind\": \"exchange\", "
               "\"item\": \"0:1:0:0\", \"count\": 1}]},\n"
               "  \"3\": {\"requirements\": [{\"kind\": \"craft\", "
               "\"item\": \"0:2:0:0\", \"count\": 1}]}\n"
               "}\n");
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadRequirementsJSON(reqs.str()), "LoadRequirementsJSON");

    // The merge also copied the kind into detectType, which is what
    // BuildQuestEraMap filters on.
    const auto *q2 = qd.GetQuest(2);
    CHECK(q2 != nullptr, "quest 2 exists");
    if (q2) {
        CHECK(q2->detectType == quest::DetectionType::EXCHANGE,
              "quest 2's kind merged into detectType as EXCHANGE");
        CHECK_EQI(q2->requirements.size(), 1, "quest 2 has one requirement");
    }
    const auto *q3 = qd.GetQuest(3);
    if (q3) {
        CHECK(q3->detectType == quest::DetectionType::CRAFT,
              "quest 3's kind merged into detectType as CRAFT");
        CHECK(q3->targetCount == 1, "quest 3's requirement count merged");
    }

    const auto eraMap = qd.BuildQuestEraMap();
    CHECK_EQI(eraMap.size(), 2, "the EXCHANGE quest is excluded from the era map");
    CHECK(eraMap.find(2) == eraMap.end(), "quest 2 (EXCHANGE) is excluded");
    CHECK(eraMap.find(1) != eraMap.end(), "quest 1 (no requirements) is included");
    CHECK(eraMap.find(3) != eraMap.end(), "quest 3 (CRAFT) is included");
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
    printf("=== quest_graph test suite ===\n\n");

    TEST(QuestData_LoadCSV_starts_with_an_empty_reverse_index);
    TEST(QuestData_LoadGraph_builds_one_edge_per_requirement);
    TEST(QuestData_LoadGraph_overwrites_prereqs_in_place);
    TEST(QuestData_LoadGraph_rejects_malformed_input);
    TEST(QuestData_LoadGraph_skips_a_quest_missing_from_the_csv);
    TEST(QuestGraph_CanComplete_requires_every_prerequisite);
    TEST(QuestGraph_LockedByPrereqs_lists_only_the_unmet);
    TEST(QuestGraph_NewlyAvailable_only_advances_LOCKED_quests);
    TEST(QuestGraph_GetUnlocked_reports_LOCKED_but_not_AVAILABLE);
    TEST(QuestGraph_cycle_is_never_detected_and_never_recurses);
    TEST(QuestData_self_dependency_is_kept_as_an_edge);
    TEST(QuestData_dangling_prerequisite_is_kept_and_blocks_forever);
    TEST(QuestGraph_IsEraComplete_ignores_other_eras_and_unknowns);
    TEST(QuestData_BuildQuestEraMap_excludes_EXCHANGE_quests);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
