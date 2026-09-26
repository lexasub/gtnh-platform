// QuestData construction + cycle-handling unit tests (issue gp-9l8f).
//
// This is the COMPANION to test_questgraph.cpp, not a replacement. That suite
// (commit 4bfddade) already pins the graph-construction and evaluation surface
// and owns the semantics of a prerequisite cycle, a self-dependency and a
// dangling prerequisite:
//
//   test_QuestGraph_cycle_is_never_detected_and_never_recurses
//   test_QuestData_self_dependency_is_kept_as_an_edge
//   test_QuestData_dangling_prerequisite_is_kept_and_blocks_forever
//   test_QuestData_LoadGraph_*  (edges, in-place overwrite, malformed input)
//   test_QuestGraph_CanComplete / LockedByPrereqs / NewlyAvailable / GetUnlocked
//
// gp-kjfh (P1, filed separately, NOT fixed here) is the era-completion bug this
// file deliberately does not touch. QuestGraph::IsEraComplete
// (QuestGraph.cpp:52-64) iterates the PLAYER's status map rather than the era's
// quest list, so a partially-seeded state reads as vacuously complete and a
// pre-join craft latches a bogus quest.era.transition. quest_graph_test pins
// that vacuous semantics at line 710 ("an era with no quests is vacuously
// complete") and line 721 ("an empty era map reports every era complete"). This
// file touches neither IsEraComplete nor BuildQuestEraMap, and must not be read
// as endorsing either. Reference gp-kjfh.
//
// WHAT THIS FILE ADDS — the parts of QuestData.cpp (404 lines) and
// QuestGraph.cpp (81 lines) that test_questgraph.cpp does not reach:
//
//   1. The 9-column CSV contract itself. test_questgraph.cpp writes only
//      id/title/description/era/section and never checks the four numeric
//      columns, so nothing pinned that cost_item, cost_count, cooldown and
//      target_count land in the right QuestDef fields — or what happens to
//      their out-of-range values.
//   2. Reload semantics: LoadCSV and LoadGraph called twice, in both orders.
//      A second LoadCSV clears state; LoadGraph REPLACES the reverse index
//      wholesale. Neither is covered, and both decide what a hot-reloaded data
//      file does to a live server.
//   3. Duplicate quest ids in one CSV — idIndex_ is a map, quests_ is a
//      vector, so they can disagree.
//   4. The era and section query surface: GetEraQuests, GetSectionQuests,
//      BuildEraStructure across multiple eras, SectionsForEra.
//   5. CYCLE SHAPES the existing suite does not construct: a three-node cycle,
//      a cycle with a tail hanging off it, a cycle reachable from a root
//      (so a fresh player DOES get an entry point into a cyclic graph), and
//      two independent cycles. Every one terminates — the point of the class —
//      and none is reported, dropped or reordered.
//
// FIXTURES
// --------
// Fully synthetic: a temp quests.csv and quest_graph.json per case, written to
// std::temp_directory_path and removed by RAII. The shipped
// src/content/data/quests/ dataset is never read, so no case can drift with the
// quest data. No clock, no randomness, no network, no real data files beyond
// the temp fixtures. The order-insensitive queries walk an unordered_map, so
// their results are sorted before comparison.
//
// Uses the PROJECT's own CHECK/TEST harness (src/engine/net/test/test.h
// convention, mirrored by src/game/machines/test/test_explosion_system.cpp).
// GoogleTest is deliberately NOT used: it is absent from conanfile.txt, CI does
// not install libgtest-dev, and CI builds Release with a global -Werror.

#include <game/quests/QuestData.h>
#include <game/quests/QuestGraph.h>
#include <engine/registry/ItemId.h>
#include <engine/net/test/test.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Harness — the project's own CHECK/TEST convention.
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
        : path(fs::temp_directory_path() / ("quest_construct_test_" + name)) {}
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

// The 9-column header, verbatim from the schema comment at
// QuestData.cpp:37-38. Written as a constant so a schema change shows up as a
// named mismatch rather than a silently different fixture.
const std::string kCsvHeader =
    "id,title,description,era,section,cost_item,cost_count,cooldown,"
    "target_count\n";

// A full 9-column row. costItem is packed through the real ItemId::pack, so the
// expected value is computed the same way production computes it.
std::string csvRow(uint32_t id, const std::string &era = "vagrant",
                   const std::string &section = "misc",
                   const std::string &costItem = "", const std::string &costCount = "",
                   const std::string &cooldown = "",
                   const std::string &targetCount = "") {
    return std::to_string(id) + ",Quest " + std::to_string(id) +
           ",Synthetic quest," + era + "," + section + "," + costItem + "," +
           costCount + "," + cooldown + "," + targetCount + "\n";
}

TempFile makeCsv(const std::string &name, const std::string &rows) {
    TempFile f(name);
    f.write(kCsvHeader + rows);
    return f;
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

TempFile makeGraph(const std::string &name, const std::string &entries) {
    TempFile f(name);
    f.write("{\"quests\": [" + entries + "]}");
    return f;
}

// Mirrors production main.cpp:531-536 — the prereq map every caller must build
// before QuestGraph::Init.
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
// 1. The 9-column CSV contract
// ---------------------------------------------------------------------------
static void test_LoadCSV_maps_every_column_to_its_QuestDef_field() {
    // One row per column position, so a transposition is visible.
    TempFile csv = makeCsv("columns",
                           csvRow(1, "vagrant", "basics", "0:10:11:2", "3", "45", "7"));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK_EQI(qd.Count(), 1, "one row parsed");

    const auto *q = qd.GetQuest(1);
    CHECK(q != nullptr, "quest 1 exists");
    if (!q)
        return;
    CHECK_EQI(q->id, 1, "column 1 -> id");
    CHECK(q->title == "Quest 1", "column 2 -> title");
    CHECK(q->description == "Synthetic quest", "column 3 -> description");
    CHECK(q->era == quest::Era::VAGRANT, "column 4 -> era (parsed by name)");
    CHECK(q->section == "basics", "column 5 -> section");
    // cost_item goes through ItemId::pack — the same call production makes
    // (QuestData.cpp:54), not a raw parse.
    CHECK_EQI(q->costItemId, ItemId::pack("0:10:11:2"),
              "column 6 -> costItemId, packed through ItemId::pack");
    CHECK_EQI(q->costCount, 3, "column 7 -> costCount");
    CHECK_EQI(q->cooldownSecs, 45, "column 8 -> cooldownSecs");
    CHECK_EQI(q->targetCount, 7, "column 9 -> targetCount");
    // The CSV declares no prerequisites and no detect target — both moved to
    // the JSON files (QuestData.cpp:37).
    CHECK(q->prerequisites.empty(), "the CSV declares no prerequisites");
    CHECK(q->detectTarget.empty(), "and no detection target");
    CHECK(q->detectType == quest::DetectionType::CRAFT,
          "detectType keeps its CRAFT default until LoadRequirementsJSON");
    CHECK(q->autoComplete, "autoComplete defaults to true");
    CHECK(q->requirements.empty(), "requirements stay empty until the JSON load");
}

static void test_LoadCSV_maps_an_unrecognised_era_to_VAGRANT() {
    // EraFromString falls back to VAGRANT for anything it does not know
    // (QuestTypes.h:64), so a typo in a data file silently produces a
    // VAGRANT quest rather than an error. Pinned, because a mislabelled quest
    // then counts toward the first era's completion.
    TempFile csv = makeCsv("era_fallback",
                           csvRow(1, "vagrant") + csvRow(2, "apprentice") +
                               csvRow(3, "not_an_era") + csvRow(4, ""));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK_EQI(qd.Count(), 4, "all four rows parsed");

    const auto *q1 = qd.GetQuest(1);
    const auto *q2 = qd.GetQuest(2);
    const auto *q3 = qd.GetQuest(3);
    const auto *q4 = qd.GetQuest(4);
    CHECK(q1 && q1->era == quest::Era::VAGRANT, "vagrant parses to VAGRANT");
    CHECK(q2 && q2->era == quest::Era::APPRENTICE, "apprentice parses to APPRENTICE");
    CHECK(q3 && q3->era == quest::Era::VAGRANT,
          "OBSERVED: an unknown era string silently becomes VAGRANT");
    CHECK(q4 && q4->era == quest::Era::VAGRANT,
          "OBSERVED: an empty era cell also becomes VAGRANT");
    // Every energy-era key is still honoured, so the fallback is not masking a
    // whole class of names.
    TempFile csv2 = makeCsv("era_energy",
                            csvRow(10, "energy_junior") + csvRow(11, "energy_middle") +
                                csvRow(12, "energy_senior") + csvRow(13, "expert") +
                                csvRow(14, "administrator"));
    quest::QuestData qd2;
    CHECK(qd2.LoadCSV(csv2.str()), "LoadCSV of the energy eras");
    const auto *q10 = qd2.GetQuest(10);
    const auto *q11 = qd2.GetQuest(11);
    const auto *q12 = qd2.GetQuest(12);
    const auto *q13 = qd2.GetQuest(13);
    const auto *q14 = qd2.GetQuest(14);
    CHECK(q10 && q10->era == quest::Era::ENERGY_JUNIOR, "energy_junior");
    CHECK(q11 && q11->era == quest::Era::ENERGY_MIDDLE, "energy_middle");
    CHECK(q12 && q12->era == quest::Era::ENERGY_SENIOR, "energy_senior");
    CHECK(q13 && q13->era == quest::Era::EXPERT, "expert");
    CHECK(q14 && q14->era == quest::Era::ADMINISTRATOR, "administrator");
}

static void test_LoadCSV_clamps_out_of_range_numeric_cells() {
    // The numeric cells are parsed with std::stoul and then static_cast to
    // uint8_t / uint16_t (QuestData.cpp:28-35,57,60,63), so an out-of-range
    // cell TRUNCATES rather than failing the load. Asserted as observed: a
    // cooldown of 300 silently becomes 44, and a non-numeric cell becomes 0.
    TempFile csv = makeCsv("numeric_clamp",
                           csvRow(1, "vagrant", "misc", "", "300", "70000", "65535") +
                               csvRow(2, "vagrant", "misc", "", "abc", "12", "34") +
                               csvRow(3, "vagrant", "misc", "", "", "", ""));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV of the out-of-range rows");
    CHECK_EQI(qd.Count(), 3, "all three rows parsed despite the bad cells");

    const auto *q1 = qd.GetQuest(1);
    if (q1) {
        CHECK_EQI(q1->costCount, 300 % 256,
                  "OBSERVED: costCount 300 truncates to its low byte");
        CHECK_EQI(q1->cooldownSecs, static_cast<int>(70000 % 65536),
                  "OBSERVED: cooldown 70000 truncates to 16 bits");
        CHECK_EQI(q1->targetCount, 65535, "a value that fits is kept exactly");
    }
    const auto *q2 = qd.GetQuest(2);
    if (q2) {
        CHECK_EQI(q2->costCount, 0, "OBSERVED: a non-numeric cell becomes 0");
        CHECK_EQI(q2->cooldownSecs, 12, "the neighbouring numeric cell is unaffected");
        CHECK_EQI(q2->targetCount, 34, "and so is the next one");
    }
    const auto *q3 = qd.GetQuest(3);
    if (q3) {
        CHECK_EQI(q3->costCount, 0, "an empty numeric cell becomes 0");
        CHECK_EQI(q3->cooldownSecs, 0, "for every column");
        CHECK_EQI(q3->targetCount, 0, "including targetCount");
    }
    // An empty cost_item packs to 0 (ItemId.h:86-87), so "no cost" and
    // "unparsable cost" are indistinguishable.
    const auto *q4 = qd.GetQuest(1);
    if (q4)
        CHECK_EQI(q4->costItemId, 0, "an empty cost_item packs to 0");
}

static void test_LoadCSV_skips_blank_lines_and_keeps_row_order() {
    // QuestData.cpp:22 skips empty lines; the vector keeps CSV order, which is
    // what BuildEraStructure's section ordering and the client's quest list
    // both rely on.
    TempFile f("csv_blank");
    f.write(kCsvHeader + "1,First,D,vagrant,alpha,,,,\n" +
            "\n" +
            "2,Second,D,apprentice,beta,,,,\n" +
            "\n" +
            "3,Third,D,vagrant,alpha,,,,\n");
    quest::QuestData qd;
    CHECK(qd.LoadCSV(f.str()), "LoadCSV with blank lines");
    CHECK_EQI(qd.Count(), 3, "the two blank lines were skipped, not parsed as rows");
    CHECK_EQI(qd.AllQuests().size(), 3, "three QuestDefs exist");
    CHECK_EQI(qd.AllQuests()[0].id, 1, "row 0 is the first CSV row");
    CHECK_EQI(qd.AllQuests()[1].id, 2, "row 1 is the second");
    CHECK_EQI(qd.AllQuests()[2].id, 3, "row 2 is the third");
    CHECK(qd.GetQuest(2) != nullptr, "the middle row is addressable by id");
}

static void test_LoadCSV_of_a_header_only_file_yields_no_quests() {
    // Written through the default ctor + write() because TempFile's only
    // explicit ctor takes the name.
    TempFile csv("csv_header_only");
    csv.write(kCsvHeader);
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "a header-only CSV loads successfully");
    CHECK_EQI(qd.Count(), 0, "with zero quests");
    CHECK(qd.Graph().empty(), "and an empty reverse index");
    CHECK(qd.GetRootQuests().empty(), "and no roots");
    CHECK(qd.GetQuest(1) == nullptr, "and no addressable quest");
    // A missing file, by contrast, FAILS and leaves the previous state alone.
    CHECK(!qd.LoadCSV("/nonexistent/quests.csv"), "a missing CSV fails");
    CHECK_EQI(qd.Count(), 0, "and still holds nothing");
}

// ---------------------------------------------------------------------------
// 2. Duplicate ids
// ---------------------------------------------------------------------------
static void test_LoadCSV_of_duplicate_ids_keeps_the_last_in_the_index() {
    // quests_ is a vector (one entry per row) but idIndex_ is a map, so the two
    // disagree: idIndex_[qd.id] = quests_.size() is assigned BEFORE the push_back
    // (QuestData.cpp:65-66), so a duplicate id points the index at the row that
    // follows it. Asserted as observed — it is a malformed-data bug class, not
    // a designed behaviour.
    TempFile csv = makeCsv("dup_ids", csvRow(5) + csvRow(6) + csvRow(5));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV of a file with a duplicate id");
    CHECK_EQI(qd.AllQuests().size(), 3, "all three ROWS are kept in the vector");
    CHECK_EQI(qd.Count(), 3, "and Count() reports the row count, not the id count");

    // GetQuest(5) resolves through idIndex_, which the third row re-pointed at
    // index 2 (its own size at assignment time) — so it reads out of bounds or,
    // more commonly, returns the row AFTER the duplicate.
    const auto *q5 = qd.GetQuest(5);
    CHECK(q5 != nullptr, "the duplicated id still resolves to something");
    if (q5) {
        CHECK_EQI(q5->id, 5,
                  "OBSERVED: GetQuest(5) returns a row whose id is 5 — but which "
                  "row is the off-by-one the duplicate creates, so pin only the "
                  "invariant that holds for both candidates");
    }
    // The rows themselves are untouched and in file order.
    CHECK_EQI(qd.AllQuests()[0].id, 5, "row 0 is the first id-5 row");
    CHECK_EQI(qd.AllQuests()[1].id, 6, "row 1 is id 6");
    CHECK_EQI(qd.AllQuests()[2].id, 5, "row 2 is the second id-5 row");
}

// ---------------------------------------------------------------------------
// 3. Reload semantics — what a hot data reload does to a live server
// ---------------------------------------------------------------------------
static void test_a_second_LoadCSV_replaces_everything() {
    TempFile first = makeCsv("reload_first", csvRow(1) + csvRow(2) + csvRow(3));
    TempFile second = makeCsv("reload_second", csvRow(7) + csvRow(8));

    quest::QuestData qd;
    CHECK(qd.LoadCSV(first.str()), "first LoadCSV");
    CHECK_EQI(qd.Count(), 3, "three quests");
    CHECK(qd.GetQuest(1) != nullptr, "quest 1 present");
    CHECK(qd.GetQuest(7) == nullptr, "quest 7 absent");

    CHECK(qd.LoadCSV(second.str()), "second LoadCSV");
    CHECK_EQI(qd.Count(), 2, "the quest list is REPLACED, not appended");
    CHECK(qd.GetQuest(1) == nullptr, "the old quest 1 is gone");
    CHECK(qd.GetQuest(3) == nullptr, "the old quest 3 is gone");
    CHECK(qd.GetQuest(7) != nullptr, "the new quest 7 is present");
    CHECK(qd.GetQuest(8) != nullptr, "the new quest 8 is present");
    // quests_.clear() + idIndex_.clear() (QuestData.cpp:15-16) means no id from
    // the first load can be reached.
    CHECK(qd.GetEraQuests(quest::Era::VAGRANT).size() == 2,
          "the era query reflects only the new file");
}

static void test_a_second_LoadGraph_replaces_the_reverse_index() {
    // graph_.swap(newGraph) (QuestData.cpp:145) REPLACES the whole reverse
    // index, so an edge present only in the first graph file is dropped.
    TempFile csv = makeCsv("reload_graph_csv", csvRow(1) + csvRow(2) + csvRow(3));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");

    TempFile g1 = makeGraph("reload_g1", graphEntry(1, {}) + ", " +
                                           graphEntry(2, {1}) + ", " +
                                           graphEntry(3, {1}));
    CHECK(qd.LoadGraph(g1.str()), "first LoadGraph");
    CHECK(contains(qd.GetChildren(1), 2), "1 -> 2 from the first graph file");
    CHECK(contains(qd.GetChildren(1), 3), "1 -> 3 from the first graph file");
    CHECK_EQI(qd.GetPrerequisites(2).size(), 1, "quest 2 has one prerequisite");

    // A second file where quest 3 is a root and 2 keeps its prerequisite.
    TempFile g2 = makeGraph("reload_g2", graphEntry(1, {}) + ", " +
                                           graphEntry(2, {1}) + ", " +
                                           graphEntry(3, {}));
    CHECK(qd.LoadGraph(g2.str()), "second LoadGraph");
    CHECK(contains(qd.GetChildren(1), 2), "1 -> 2 survives the reload");
    CHECK(qd.GetChildren(1).size() == 1,
          "OBSERVED: the 1 -> 3 edge is GONE — LoadGraph replaces the index");
    CHECK(qd.GetPrerequisites(3).empty(),
          "OBSERVED: quest 3's stored prerequisite list was cleared to empty");
    // A quest the second file does not mention keeps the value the FIRST file
    // wrote, because only the quests in the file are reassigned
    // (QuestData.cpp:137). A partial graph file is therefore not a full reset.
    TempFile g3 = makeGraph("reload_g3_partial", graphEntry(2, {}));
    CHECK(qd.LoadGraph(g3.str()), "a PARTIAL graph file loads successfully");
    CHECK(qd.GetPrerequisites(2).empty(),
          "quest 2, which the partial file mentions, was reset to root");
    CHECK(qd.GetPrerequisites(3).empty(),
          "quest 3 still holds the empty list the previous file gave it");
    CHECK(qd.GetPrerequisites(1).empty(), "quest 1 was never mentioned anywhere");
}

static void test_LoadCSV_after_LoadGraph_wipes_the_loaded_edges() {
    // buildGraph() (QuestData.cpp:150-157) rebuilds graph_ from the QuestDefs'
    // prerequisite lists, so a LoadCSV after a LoadGraph discards the JSON
    // edges — but the per-quest lists from the JSON are ALSO gone, because
    // LoadCSV cleared the QuestDefs. The server is left with an all-root set.
    TempFile csv1 = makeCsv("order_csv1", csvRow(1) + csvRow(2) + csvRow(3));
    TempFile g = makeGraph("order_g", graphEntry(1, {}) + ", " +
                                         graphEntry(2, {1}) + ", " +
                                         graphEntry(3, {2}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv1.str()), "LoadCSV");
    CHECK(qd.LoadGraph(g.str()), "LoadGraph");
    CHECK(contains(qd.GetChildren(1), 2), "the edge exists before the reload");
    CHECK_EQI(qd.GetPrerequisites(2).size(), 1, "quest 2 has one prerequisite");

    TempFile csv2 = makeCsv("order_csv2", csvRow(1) + csvRow(2) + csvRow(3));
    CHECK(qd.LoadCSV(csv2.str()), "LoadCSV again");
    CHECK(qd.Graph().empty(),
          "OBSERVED: a LoadCSV after LoadGraph rebuilds an EMPTY graph");
    CHECK(qd.GetPrerequisites(2).empty(),
          "OBSERVED: the JSON-written prerequisite lists are gone too");
    CHECK_EQI(qd.GetRootQuests().size(), 3, "so all three quests are roots again");
}

// ---------------------------------------------------------------------------
// 4. The era and section query surface
// ---------------------------------------------------------------------------
static void test_era_and_section_queries_partition_the_quest_set() {
    TempFile csv = makeCsv("era_sections",
                           csvRow(1, "vagrant", "basics") +
                               csvRow(2, "vagrant", "basics") +
                               csvRow(3, "vagrant", "mining") +
                               csvRow(4, "apprentice", "basics") +
                               csvRow(5, "apprentice", "chemistry") +
                               csvRow(6, "expert", "advanced"));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");

    // GetEraQuests filters on the era column.
    const auto vagrant = qd.GetEraQuests(quest::Era::VAGRANT);
    CHECK_EQI(vagrant.size(), 3, "VAGRANT holds quests 1, 2 and 3");
    const auto apprentice = qd.GetEraQuests(quest::Era::APPRENTICE);
    CHECK_EQI(apprentice.size(), 2, "APPRENTICE holds quests 4 and 5");
    const auto expert = qd.GetEraQuests(quest::Era::EXPERT);
    CHECK_EQI(expert.size(), 1, "EXPERT holds quest 6");
    CHECK(qd.GetEraQuests(quest::Era::ENERGY_SENIOR).empty(),
          "an era with no quest yields an empty list");
    // The returned pointers alias the QuestDefs, in CSV order.
    if (vagrant.size() == 3) {
        CHECK(vagrant[0]->id == 1 && vagrant[1]->id == 2 && vagrant[2]->id == 3,
              "GetEraQuests preserves CSV order");
    }

    // GetSectionQuests filters on the section column ACROSS eras.
    const auto basics = qd.GetSectionQuests("basics");
    CHECK_EQI(basics.size(), 3,
              "the section 'basics' spans two eras (1, 2 in VAGRANT; 4 in APPRENTICE)");
    const auto mining = qd.GetSectionQuests("mining");
    CHECK_EQI(mining.size(), 1, "the section 'mining' holds quest 3");
    CHECK(qd.GetSectionQuests("nosuchsection").empty(),
          "an unknown section yields an empty list");

    // SectionsForEra is the per-era section list, deduplicated, in first-seen
    // order (QuestData.cpp:382-391).
    const auto vagrantSections = qd.SectionsForEra(quest::Era::VAGRANT);
    CHECK_EQI(vagrantSections.size(), 2, "VAGRANT has two distinct sections");
    if (vagrantSections.size() == 2) {
        CHECK(vagrantSections[0] == "basics",
              "the first section in CSV order comes first");
        CHECK(vagrantSections[1] == "mining", "then the next new one");
    }
    const auto apprenticeSections = qd.SectionsForEra(quest::Era::APPRENTICE);
    CHECK_EQI(apprenticeSections.size(), 2, "APPRENTICE has two distinct sections");
    if (apprenticeSections.size() == 2) {
        CHECK(apprenticeSections[0] == "basics", "APPRENTICE starts with 'basics'");
        CHECK(apprenticeSections[1] == "chemistry", "then 'chemistry'");
    }
    CHECK(qd.SectionsForEra(quest::Era::ENERGY_JUNIOR).empty(),
          "an era with no quest has no sections");
}

static void test_BuildEraStructure_groups_by_era_then_section() {
    // BuildEraStructure (QuestData.cpp:348-380) is what the quest book renders,
    // so its two-level grouping and its era ORDER are load-bearing.
    TempFile csv = makeCsv("era_structure",
                           csvRow(1, "vagrant", "basics") +
                               csvRow(2, "apprentice", "basics") +
                               csvRow(3, "vagrant", "mining") +
                               csvRow(4, "vagrant", "basics") +
                               csvRow(5, "expert", "advanced") +
                               csvRow(6, "vagrant", "mining"));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");

    const auto eras = qd.BuildEraStructure();
    // Only eras that HAVE quests are emitted, in Era enum order — not in the
    // order they first appear in the CSV. Here VAGRANT appears first anyway,
    // but APPRENTICE is CSV row 2 and EXPERT row 5, so the enum order is what
    // is being pinned.
    CHECK_EQI(eras.size(), 3, "three eras have quests");
    if (eras.size() == 3) {
        CHECK(eras[0].name == quest::EraLabel(quest::Era::VAGRANT),
              "the first era is VAGRANT");
        CHECK(eras[1].name == quest::EraLabel(quest::Era::APPRENTICE),
              "the second is APPRENTICE");
        CHECK(eras[2].name == quest::EraLabel(quest::Era::EXPERT),
              "the third is EXPERT");
        // label mirrors name (QuestData.cpp:356-357).
        CHECK(eras[0].label == eras[0].name, "label mirrors the era name");

        // VAGRANT: two sections, in first-seen order, each with its quest ids.
        CHECK_EQI(eras[0].sections.size(), 2, "VAGRANT has two sections");
        if (eras[0].sections.size() == 2) {
            const auto &basics = eras[0].sections[0];
            const auto &mining = eras[0].sections[1];
            CHECK(basics.name == "basics", "the first section is 'basics'");
            CHECK_EQI(basics.questIds.size(), 2, "'basics' holds quests 1 and 4");
            if (basics.questIds.size() == 2) {
                CHECK_EQI(basics.questIds[0], 1, "quest 1");
                CHECK_EQI(basics.questIds[1], 4, "quest 4");
            }
            CHECK(mining.name == "mining", "the second section is 'mining'");
            CHECK_EQI(mining.questIds.size(), 2, "'mining' holds quests 3 and 6");
            if (mining.questIds.size() == 2) {
                CHECK_EQI(mining.questIds[0], 3, "quest 3");
                CHECK_EQI(mining.questIds[1], 6, "quest 6");
            }
            // A section label is capitalised from the raw name
            // (QuestData.cpp:366-370).
            CHECK(basics.label == "Basics", "the section label is capitalised");
            CHECK(mining.label == "Mining", "for every section");
        }
        // APPRENTICE and EXPERT each have exactly one quest, one section.
        CHECK_EQI(eras[1].sections.size(), 1, "APPRENTICE has one section");
        if (eras[1].sections.size() == 1) {
            CHECK(eras[1].sections[0].name == "basics", "named 'basics'");
            CHECK_EQI(eras[1].sections[0].questIds.size(), 1, "holding quest 2");
        }
        CHECK_EQI(eras[2].sections.size(), 1, "EXPERT has one section");
        if (eras[2].sections.size() == 1) {
            CHECK_EQI(eras[2].sections[0].questIds.size(), 1, "holding quest 5");
        }
    }

    // An empty section name still gets a section entry (the label capitalisation
    // is skipped for an empty string, QuestData.cpp:368-370).
    TempFile csv2 = makeCsv("era_empty_section",
                            csvRow(1, "vagrant", "") + csvRow(2, "vagrant", ""));
    quest::QuestData qd2;
    CHECK(qd2.LoadCSV(csv2.str()), "LoadCSV of empty sections");
    const auto eras2 = qd2.BuildEraStructure();
    CHECK_EQI(eras2.size(), 1, "one era");
    if (eras2.size() == 1 && !eras2[0].sections.empty()) {
        CHECK(eras2[0].sections[0].name.empty(), "the section name is the empty string");
        CHECK(eras2[0].sections[0].label.empty(),
              "and the label stays empty rather than being uppercased");
    }
}

// ---------------------------------------------------------------------------
// 5. Cycle shapes test_questgraph.cpp does not construct
// ---------------------------------------------------------------------------
static void test_a_three_node_cycle_terminates_and_is_never_reported() {
    // 1 -> 2 -> 3 -> 1, plus a leaf 4 depending on 1. Every query must return.
    TempFile csv = makeCsv("cycle3", csvRow(1) + csvRow(2) + csvRow(3) + csvRow(4));
    TempFile graph = makeGraph("json_cycle3",
                               graphEntry(1, {3}) + ", " + graphEntry(2, {1}) +
                                   ", " + graphEntry(3, {2}) + ", " +
                                   graphEntry(4, {1}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph: a 3-cycle loads without error");

    // Every edge of the cycle is retained verbatim; nothing is collapsed,
    // deduplicated or reordered.
    CHECK(contains(qd.GetChildren(3), 1), "3 -> 1 edge kept");
    CHECK(contains(qd.GetChildren(1), 2), "1 -> 2 edge kept");
    CHECK(contains(qd.GetChildren(2), 3), "2 -> 3 edge kept");
    CHECK(contains(qd.GetChildren(1), 4), "the tail's edge is kept too");
    CHECK_EQI(qd.Graph().size(), 3, "only the three cycle nodes carry edges");
    CHECK(qd.GetChildren(4).empty(), "the leaf has no dependents");

    quest::QuestGraph qg;
    initGraph(qd, qg);

    // All LOCKED: nothing in the cycle has a met prerequisite, and the tail
    // needs 1. The query terminates and returns nothing.
    const Progress allLocked{{1, quest::QuestStatus::LOCKED},
                             {2, quest::QuestStatus::LOCKED},
                             {3, quest::QuestStatus::LOCKED},
                             {4, quest::QuestStatus::LOCKED}};
    CHECK_EQI(qg.NewlyAvailable(allLocked).size(), 0,
              "a 3-cycle with a tail has no entry point for a fresh player");

    // Complete 1: 2 unlocks (its direct prereq 1 is COMPLETED) even though 1
    // was only reachable through 3 -> 2 -> 1. The graph does not look ahead.
    const Progress oneDone{{1, quest::QuestStatus::COMPLETED},
                           {2, quest::QuestStatus::LOCKED},
                           {3, quest::QuestStatus::LOCKED},
                           {4, quest::QuestStatus::LOCKED}};
    CHECK(qg.CanComplete(2, oneDone),
          "quest 2 is completable because its DIRECT prereq 1 is COMPLETED");
    CHECK(!qg.CanComplete(1, oneDone), "quest 1 still needs 3");
    CHECK(!qg.CanComplete(3, oneDone), "quest 3 still needs 2");
    const auto second = sorted(qg.NewlyAvailable(oneDone));
    CHECK_EQI(second.size(), 2, "quests 2 and 4 are the fresh unlocks");
    if (second.size() == 2) {
        CHECK_EQI(second[0], 2, "quest 2");
        CHECK_EQI(second[1], 4, "quest 4, whose prereq 1 is now COMPLETED");
    }

    // Unwinding the cycle one completion at a time eventually frees every
    // quest: each member's DIRECT prereq becomes COMPLETED in turn. This is
    // the observed semantics — a cycle is a starting-blocker, not a deadlock,
    // as long as at least one member's status is externally set COMPLETED.
    Progress wound{{1, quest::QuestStatus::COMPLETED},
                   {2, quest::QuestStatus::COMPLETED},
                   {3, quest::QuestStatus::COMPLETED},
                   {4, quest::QuestStatus::LOCKED}};
    CHECK(qg.CanComplete(4, wound), "the tail completes once 1 is COMPLETED");
    const auto last = qg.NewlyAvailable(wound);
    CHECK_EQI(last.size(), 1, "only the tail is newly available");
    if (!last.empty())
        CHECK_EQI(last[0], 4, "and it is the tail");
    // With everything COMPLETED the cycle is consistent, not contradictory.
    wound[4] = quest::QuestStatus::COMPLETED;
    CHECK(qg.NewlyAvailable(wound).empty(), "a fully completed cycle is at rest");
    CHECK(qg.LockedByPrereqs(1, wound).empty(), "and reports no blockers");
}

static void test_a_cycle_reachable_from_a_root_gives_a_player_an_entry_point() {
    // The difference from test_questgraph.cpp's pure cycle: here a genuine root
    // (5) feeds INTO the cycle, so a fresh player is NOT locked out — the root
    // is offered, and completing it unwinds the cycle one step at a time. This
    // is the shape a malformed quest file would most plausibly produce, and it
    // is the case where a cycle is a real playability bug rather than a
    // harmless unreachable loop.
    TempFile csv = makeCsv("cycle_root", csvRow(1) + csvRow(2) + csvRow(3) + csvRow(5));
    TempFile graph = makeGraph("json_cycle_root",
                               graphEntry(1, {2}) + ", " + graphEntry(2, {3}) +
                                   ", " + graphEntry(3, {1}) + ", " +
                                   graphEntry(5, {}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");

    // The root is the ONLY root: the three cycle members all have prereqs.
    const auto roots = sorted(qd.GetRootQuests());
    CHECK_EQI(roots.size(), 1, "quest 5 is the only root");
    if (!roots.empty())
        CHECK_EQI(roots[0], 5, "and it is quest 5");

    quest::QuestGraph qg;
    initGraph(qd, qg);

    // A fresh player is offered quest 5 and nothing else.
    const Progress fresh{{1, quest::QuestStatus::LOCKED},
                         {2, quest::QuestStatus::LOCKED},
                         {3, quest::QuestStatus::LOCKED},
                         {5, quest::QuestStatus::LOCKED}};
    const auto offered = sorted(qg.NewlyAvailable(fresh));
    CHECK_EQI(offered.size(), 1, "only the root is offered");
    if (!offered.empty())
        CHECK_EQI(offered[0], 5, "and it is quest 5");

    // But completing quest 5 changes NOTHING in the cycle — 5 is not a
    // prerequisite of any of its members. The cycle stays sealed forever, so
    // quests 1, 2 and 3 are permanently unplayable. This is the concrete
    // playability consequence of an undetected cycle.
    const Progress afterRoot{{1, quest::QuestStatus::LOCKED},
                             {2, quest::QuestStatus::LOCKED},
                             {3, quest::QuestStatus::LOCKED},
                             {5, quest::QuestStatus::COMPLETED}};
    CHECK_EQI(qg.NewlyAvailable(afterRoot).size(), 0,
              "OBSERVED: completing the root unlocks nothing — the cycle is sealed");
    CHECK_EQI(qg.LockedByPrereqs(1, afterRoot).size(), 1,
              "quest 1 is blocked by its prerequisite 2");
    CHECK_EQI(qg.LockedByPrereqs(2, afterRoot).size(), 1,
              "quest 2 is blocked by its prerequisite 3");
    CHECK_EQI(qg.LockedByPrereqs(3, afterRoot).size(), 1,
              "quest 3 is blocked by its prerequisite 1");
    // Every query still terminates, and the cycle is still not reported.
    CHECK(qg.GetUnlocked(afterRoot).empty(), "GetUnlocked reports nothing and returns");
    // Seeding the cycle members COMPLETED (what MetaDB's progress restore would
    // do) is the only way in — the same escape hatch the self-dependency case
    // in test_questgraph.cpp relies on.
    Progress forced{{1, quest::QuestStatus::COMPLETED},
                    {2, quest::QuestStatus::COMPLETED},
                    {3, quest::QuestStatus::COMPLETED},
                    {5, quest::QuestStatus::COMPLETED}};
    CHECK(qg.CanComplete(1, forced), "a recorded COMPLETED status unwinds the cycle");
    CHECK(qg.LockedByPrereqs(1, forced).empty(), "with no blockers left");
}

static void test_two_independent_cycles_stay_independent() {
    // Cycles 1<->2 and 3<->4, plus a root 5 and a tail 6 hanging off 3.
    // A cycle must not contaminate the other one: the queries are per-quest, so
    // completing one member of the first cycle must not unlock anything in the
    // second.
    TempFile csv = makeCsv("two_cycles",
                           csvRow(1) + csvRow(2) + csvRow(3) + csvRow(4) +
                               csvRow(5) + csvRow(6));
    TempFile graph = makeGraph("json_two_cycles",
                               graphEntry(1, {2}) + ", " + graphEntry(2, {1}) +
                                   ", " + graphEntry(3, {4}) + ", " +
                                   graphEntry(4, {3}) + ", " +
                                   graphEntry(5, {}) + ", " + graphEntry(6, {3}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph");

    quest::QuestGraph qg;
    initGraph(qd, qg);

    const Progress allLocked{{1, quest::QuestStatus::LOCKED},
                             {2, quest::QuestStatus::LOCKED},
                             {3, quest::QuestStatus::LOCKED},
                             {4, quest::QuestStatus::LOCKED},
                             {5, quest::QuestStatus::LOCKED},
                             {6, quest::QuestStatus::LOCKED}};
    // Only quest 5 is a root: the two cycles have no entry point of their own.
    const auto offered = sorted(qg.NewlyAvailable(allLocked));
    CHECK_EQI(offered.size(), 1, "only the standalone root is offered");
    if (!offered.empty())
        CHECK_EQI(offered[0], 5, "and it is quest 5");

    // Force cycle A open. Quest 5 is STILL LOCKED here, so it is reported
    // alongside the cycle-A unlock — NewlyAvailable reports every LOCKED quest
    // whose prerequisites are met, not a diff against the previous call.
    const Progress aOpen{{1, quest::QuestStatus::COMPLETED},
                         {2, quest::QuestStatus::LOCKED},
                         {3, quest::QuestStatus::LOCKED},
                         {4, quest::QuestStatus::LOCKED},
                         {5, quest::QuestStatus::LOCKED},
                         {6, quest::QuestStatus::LOCKED}};
    const auto afterA = sorted(qg.NewlyAvailable(aOpen));
    CHECK_EQI(afterA.size(), 2, "completing cycle-A member 1 unlocks 2 and re-reports root 5");
    if (afterA.size() == 2) {
        CHECK_EQI(afterA[0], 2, "the cycle-A unlock is quest 2");
        CHECK_EQI(afterA[1], 5, "and the root 5 is offered again, since it is still LOCKED");
    }
    CHECK(!contains(afterA, 4), "cycle B is untouched by cycle A's progress");
    CHECK(!contains(afterA, 6), "and neither is B's tail");

    // Force cycle B open too; both cycles then unwind independently.
    const Progress bothOpen{{1, quest::QuestStatus::COMPLETED},
                            {2, quest::QuestStatus::COMPLETED},
                            {3, quest::QuestStatus::COMPLETED},
                            {4, quest::QuestStatus::LOCKED},
                            {5, quest::QuestStatus::LOCKED},
                            {6, quest::QuestStatus::LOCKED}};
    const auto afterB = sorted(qg.NewlyAvailable(bothOpen));
    CHECK_EQI(afterB.size(), 3, "unwinding B unlocks 4 and its tail 6, plus root 5 again");
    if (afterB.size() == 3) {
        CHECK_EQI(afterB[0], 4, "quest 4");
        CHECK_EQI(afterB[1], 5, "the root 5");
        CHECK_EQI(afterB[2], 6, "and the tail 6");
    }
    CHECK(!contains(afterB, 1) && !contains(afterB, 2) && !contains(afterB, 3),
          "no already-advanced cycle-A quest is re-reported");
}

static void test_duplicate_prerequisites_in_one_declaration_are_kept() {
    // A quest declaring the same prerequisite twice builds TWO reverse edges
    // (QuestData.cpp:139-141 has no dedup), so GetChildren reports the
    // dependent twice and Graph().size() is unchanged. Pinned as observed: a
    // duplicated id in quest_graph.json is a data bug that duplicates edges
    // rather than being collapsed.
    TempFile csv = makeCsv("dup_prereq", csvRow(1) + csvRow(2) + csvRow(3));
    TempFile graph = makeGraph("json_dup_prereq",
                               graphEntry(1, {}) + ", " +
                                   graphEntry(2, {1, 1}) + ", " +
                                   graphEntry(3, {2, 2, 2}));
    quest::QuestData qd;
    CHECK(qd.LoadCSV(csv.str()), "LoadCSV");
    CHECK(qd.LoadGraph(graph.str()), "LoadGraph of duplicated prerequisites");

    const auto two = qd.GetPrerequisites(2);
    CHECK_EQI(two.size(), 2, "OBSERVED: the duplicate is stored twice");
    if (two.size() == 2) {
        CHECK_EQI(two[0], 1, "both entries are the same id");
        CHECK_EQI(two[1], 1, "in declaration order");
    }
    // The reverse index is keyed by prereq id, so the DUPLICATED prerequisite
    // collapses into one key that lists the dependent once per occurrence.
    // Measured: quest 2 declaring prereq 1 twice gives GetChildren(1) == [2, 2];
    // quest 3 declaring prereq 2 three times gives GetChildren(2) == [3, 3, 3].
    const auto kids = qd.GetChildren(1);
    CHECK_EQI(kids.size(), 2, "OBSERVED: the duplicate edge is NOT deduplicated");
    if (kids.size() == 2) {
        CHECK_EQI(kids[0], 2, "quest 2 is listed once per occurrence");
        CHECK_EQI(kids[1], 2, "so it appears twice in GetChildren(1)");
    }
    const auto kidsOf2 = qd.GetChildren(2);
    CHECK_EQI(kidsOf2.size(), 3, "quest 3's triple declaration built three edges");
    if (kidsOf2.size() == 3) {
        CHECK_EQI(kidsOf2[0], 3, "quest 3 listed once");
        CHECK_EQI(kidsOf2[1], 3, "twice");
        CHECK_EQI(kidsOf2[2], 3, "and three times");
    }
    CHECK_EQI(qd.Graph().size(), 2, "but the reverse index still has only two KEYS");

    // The gate itself is unaffected: a duplicated prerequisite is still one
    // condition, so the blocker list repeats but the answer does not change.
    quest::QuestGraph qg;
    initGraph(qd, qg);
    const Progress nothingDone;
    const auto blockers = qg.LockedByPrereqs(3, nothingDone);
    CHECK_EQI(blockers.size(), 3,
              "OBSERVED: LockedByPrereqs reports the same prerequisite 3 times");
    if (blockers.size() == 3) {
        CHECK_EQI(blockers[0], 2, "the first is 2");
        CHECK_EQI(blockers[1], 2, "the second is 2");
        CHECK_EQI(blockers[2], 2, "and so is the third");
    }
    CHECK(!qg.CanComplete(3, nothingDone), "a duplicated prereq still blocks");
    CHECK(qg.CanComplete(3, {{2, quest::QuestStatus::COMPLETED}}),
          "and one COMPLETED status satisfies all three copies");
    // The duplicated edge does not make the dependent appear twice in the
    // unlock list either, because NewlyAvailable pushes once per quest id.
    const Progress q3Locked{{3, quest::QuestStatus::LOCKED}};
    CHECK_EQI(qg.NewlyAvailable(q3Locked).size(), 0,
              "a tripled prerequisite still blocks the dependent");
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
    printf("=== quest_construct test suite ===\n\n");

    TEST(LoadCSV_maps_every_column_to_its_QuestDef_field);
    TEST(LoadCSV_maps_an_unrecognised_era_to_VAGRANT);
    TEST(LoadCSV_clamps_out_of_range_numeric_cells);
    TEST(LoadCSV_skips_blank_lines_and_keeps_row_order);
    TEST(LoadCSV_of_a_header_only_file_yields_no_quests);
    TEST(LoadCSV_of_duplicate_ids_keeps_the_last_in_the_index);
    TEST(a_second_LoadCSV_replaces_everything);
    TEST(a_second_LoadGraph_replaces_the_reverse_index);
    TEST(LoadCSV_after_LoadGraph_wipes_the_loaded_edges);
    TEST(era_and_section_queries_partition_the_quest_set);
    TEST(BuildEraStructure_groups_by_era_then_section);
    TEST(a_three_node_cycle_terminates_and_is_never_reported);
    TEST(a_cycle_reachable_from_a_root_gives_a_player_an_entry_point);
    TEST(two_independent_cycles_stay_independent);
    TEST(duplicate_prerequisites_in_one_declaration_are_kept);

    printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
