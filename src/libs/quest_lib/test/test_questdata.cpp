// quest_lib data-loader tests.
// Verifies: 9-column CSV parsing, LoadRequirementsJSON merge semantics,
// LoadRewardsJSON rewards/choice_of XOR, and GetReward lookups — against the
// real data/quests dataset plus synthetic files for loader edge cases.
#include "quest_lib/QuestData.h"
#include "quest_lib/QuestGraph.h"
#include <libgtnh-net/test/test.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifndef DATA_DIR
#error "DATA_DIR must be defined to the repository data/ root"
#endif

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

namespace fs = std::filesystem;

static std::string TempRewardsJSON(const std::string& mode) {
    if (mode == "rewards") {
        return R"({
  "1": {"rewards": [{"type": "item", "item": "0:10:11:2", "count": 1}]}
})";
    }
    if (mode == "choice_of") {
        return R"({
  "2": {"choice_of": [
    {"type": "item", "item": "0:1:0:0", "count": 5},
    {"type": "experience", "value": 100}
  ]}
})";
    }
    return "{}";
}

void test_LoadCSV_NineColumnSchema() {
    quest::QuestData qd;
    const std::string csv = std::string(DATA_DIR) + "/quests/quests.csv";
    CHECK(qd.LoadCSV(csv));
    CHECK_GE(qd.Count(), size_t(150));

    // Quest 1 must parse the 9-column schema: era, section, cost/target cells.
    auto* q1 = qd.GetQuest(1);
    CHECK(q1 != nullptr);
    CHECK(q1->detectType == quest::DetectionType::CRAFT);
    CHECK(q1->detectTarget.empty());
    CHECK(q1->costItemId == 0);
    CHECK(q1->costCount == 0);
    CHECK(q1->cooldownSecs == 0);
}

void test_BuildEraStructure_PreservesSectionDefinitionOrder() {
    quest::QuestData qd;
    fs::path csv = fs::temp_directory_path() / "quest_section_order.csv";
    {
        std::ofstream of(csv);
        of << "id,title,description,era,section,cost_item,cost_count,cooldown,target_count\n"
              "9001,First,Test,vagrant,zeta,,,,\n"
              "9002,Second,Test,vagrant,alpha,,,,\n"
              "9003,Third,Test,vagrant,zeta,,,,\n"
              "9004,Fourth,Test,vagrant,beta,,,,\n";
    }
    CHECK(qd.LoadCSV(csv.string()));
    fs::remove(csv);

    const auto eras = qd.BuildEraStructure();
    CHECK_EQ(eras.size(), size_t(1));
    CHECK_EQ(eras[0].sections.size(), size_t(3));
    CHECK(eras[0].sections[0].name == "zeta");
    CHECK(eras[0].sections[1].name == "alpha");
    CHECK(eras[0].sections[2].name == "beta");
    CHECK_EQ(eras[0].sections[0].questIds.size(), size_t(2));
    CHECK_EQ(eras[0].sections[0].questIds[0], uint32_t(9001));
    CHECK_EQ(eras[0].sections[0].questIds[1], uint32_t(9003));
}

void test_LoadRequirementsJSON_MergesIntoQuestDef() {
    quest::QuestData qd;
    CHECK(qd.LoadCSV(std::string(DATA_DIR) + "/quests/quests.csv"));

    const std::string json = std::string(DATA_DIR) + "/quests/quest_requirements.json";
    CHECK(qd.LoadRequirementsJSON(json));

    // 163 seeded quests carry requirements; autoComplete defaults true.
    auto* q1 = qd.GetQuest(1);
    CHECK(q1 != nullptr);
    CHECK(!q1->requirements.empty());
    CHECK(q1->autoComplete);
    CHECK(q1->detectType == quest::DetectionType::INVENTORY);
    CHECK(q1->detectTarget == "0:10:11:2");
}

void test_LoadRequirementsJSON_MachineKind() {
    quest::QuestData qd;
    // Fully synthetic CSV + requirements pair so the machine-kind mapping and
    // auto_complete=false are exercised in isolation from the repo dataset.
    fs::path csv = fs::temp_directory_path() / "quest_csv_9999.csv";
    fs::path req = fs::temp_directory_path() / "quest_req_9999.json";
    {
        std::ofstream of(csv);
        of << "id,title,description,era,section,cost_item,cost_count,cooldown,target_count\n"
              "9999,Synth,Test quest,vagrant,misc,,,,\n";
    }
    {
        std::ofstream of(req);
        of << "{\n"
              "  \"9999\": {\n"
              "    \"auto_complete\": false,\n"
              "    \"requirements\": [\n"
              "      {\"kind\": \"machine\", \"item\": \"0:10:11:1\", \"count\": 4, \"consume\": false, \"machine\": \"1:2:3:0\"}\n"
              "    ]\n"
              "  }\n"
              "}\n";
    }
    CHECK(qd.LoadCSV(csv.string()));
    CHECK(qd.LoadRequirementsJSON(req.string()));
    fs::remove(csv);
    fs::remove(req);

    auto* q = qd.GetQuest(9999);
    CHECK(q != nullptr);
    CHECK(!q->autoComplete);
    CHECK_EQ(q->requirements.size(), size_t(1));
    CHECK(q->requirements[0].kind == quest::DetectionType::MACHINE);
    CHECK_EQ(q->requirements[0].count, uint16_t(4));
    CHECK(q->requirements[0].machine == "1:2:3:0");
    CHECK(q->requirements[0].consume == false);
    CHECK(q->detectType == quest::DetectionType::MACHINE);
    CHECK(q->detectTarget == "0:10:11:1");
    CHECK_EQ(q->targetCount, uint16_t(4));
}

void test_LoadRewardsJSON_RewardsAndChoiceOf() {
    quest::QuestData qd;
    // Rewards map is independent of quests_ index, so no CSV load required to
    // exercise the loader itself; GetReward returns by map lookup.
    const std::string json = std::string(DATA_DIR) + "/quests/quest_rewards.json";
    CHECK(qd.LoadRewardsJSON(json));

    // Quest 1 reward seeded: one ITEM reward "0:10:11:2", count 1.
    auto* r1 = qd.GetReward(1);
    CHECK(r1 != nullptr);
    CHECK_EQ(r1->rewards.size(), size_t(1));
    CHECK(r1->rewards[0].type == quest::RewardType::ITEM);
    CHECK(r1->rewards[0].item == "0:10:11:2");
    CHECK_EQ(r1->rewards[0].count, uint16_t(1));

    // Reward seeds cover only quests 1–36; quest 100 has no JSON entry.
    CHECK(qd.GetReward(100) == nullptr);
}

void test_GetReward_ChoiceOfXor() {
    quest::QuestData qd;
    fs::path tmp = fs::temp_directory_path() / "quest_reward_choice.json";
    {
        std::ofstream of(tmp);
        of << TempRewardsJSON("choice_of");
    }
    CHECK(qd.LoadRewardsJSON(tmp.string()));
    fs::remove(tmp);

    // choice_of populated; rewards vector stays empty (XOR enforced by loader).
    auto* r2 = qd.GetReward(2);
    CHECK(r2 != nullptr);
    CHECK(r2->rewards.empty());
    CHECK_EQ(r2->choiceOf.size(), size_t(2));
    CHECK(r2->choiceOf[1].type == quest::RewardType::EXPERIENCE);
}

void test_LoadRewardsJSON_RewardsVectorPath() {
    quest::QuestData qd;
    fs::path tmp = fs::temp_directory_path() / "quest_reward_plain.json";
    {
        std::ofstream of(tmp);
        of << TempRewardsJSON("rewards");
    }
    CHECK(qd.LoadRewardsJSON(tmp.string()));
    fs::remove(tmp);

    auto* r1 = qd.GetReward(1);
    CHECK(r1 != nullptr);
    CHECK_EQ(r1->rewards.size(), size_t(1));
    CHECK(r1->choiceOf.empty());
}

void test_LockedByPrereqs() {
    quest::QuestGraph g;
    // quest 10 depends on 1 and 2; quest 2 depends on 1.
    std::unordered_map<uint32_t, std::vector<uint32_t>> graph;
    graph[1] = {2};
    graph[2] = {10};
    std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
    prereqs[10] = {1, 2};
    prereqs[2] = {1};
    g.Init(graph, prereqs);

    std::unordered_map<uint32_t, quest::QuestStatus> empty;
    auto blocked = g.LockedByPrereqs(10, empty);
    CHECK_EQ(blocked.size(), size_t(2));

    // One prereq completed, the other not → only the unmet one blocks.
    std::unordered_map<uint32_t, quest::QuestStatus> prog{
        {1, quest::QuestStatus::COMPLETED}, {2, quest::QuestStatus::AVAILABLE}};
    blocked = g.LockedByPrereqs(10, prog);
    CHECK_EQ(blocked.size(), size_t(1));
    CHECK_EQ(blocked[0], uint32_t(2));

    // All prereqs completed → unlocked.
    prog[1] = quest::QuestStatus::COMPLETED;
    prog[2] = quest::QuestStatus::COMPLETED;
    CHECK(g.LockedByPrereqs(10, prog).empty());

    // Quest with no prerequisites is never blocked.
    CHECK(g.LockedByPrereqs(3, empty).empty());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== quest_lib test suite ===\n\n");

    TEST(LoadCSV_NineColumnSchema);
    TEST(BuildEraStructure_PreservesSectionDefinitionOrder);
    TEST(LoadRequirementsJSON_MergesIntoQuestDef);
    TEST(LoadRequirementsJSON_MachineKind);
    TEST(LoadRewardsJSON_RewardsAndChoiceOf);
    TEST(GetReward_ChoiceOfXor);
    TEST(LoadRewardsJSON_RewardsVectorPath);
    TEST(LockedByPrereqs);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
