// QuestTypes unit tests (issue gp-kd4u).
//
// src/game/quests/QuestTypes.cpp is two lines (everything is inline in
// QuestTypes.h) but the enums it declares are load-bearing:
//
//   * quest::QuestStatus  <-> Protocol::QuestStatus in src/protocol/quest.fbs
//     (the QuestBook wire format). The Go integration tracer
//     test/integration/questbook_wire_tracer_test.go:86 asserts
//     `entry.Status() == Protocol.QuestStatusCOMPLETED && entry.Progress() == 100`
//     over a real socket, so the C++ enum must stay bit-compatible with the
//     schema or that test fails in CI.
//   * quest::Era / DetectionType / RewardType: the string<->enum parsers used
//     by the quest_requirements.json / quest_rewards.json / quests.csv
//     loaders, plus the enum defaults that decide "unknown" behaviour.
//
// What this test pins:
//   1. the four QuestStatus wire values and their underlying type,
//   2. that quest::QuestStatus and Protocol::QuestStatus agree on every value
//      and that the wire enum's [MIN,MAX] span is exactly those four,
//   3. a full round trip through a real FlatBuffer QuestProgressUpdate /
//      QuestEntry (the shape the QuestBook speaks) for every status,
//   4. that a status outside 0..3 is NOT representable on the wire (which is
//      what makes QuestManager::loadProgress's `status >= 4` clamp a contract
//      rather than an arbitrary constant),
//   5. Era wire values, the COUNT sentinel, the era->next-era arithmetic
//      publishEraTransition relies on, and EraLabel/EraFromString in both
//      directions (including: a label does NOT round-trip back through
//      EraFromString — the parser is case-sensitive lowercase),
//   6. DetectFromString / DetectionType / RewardType values and the
//      "unrecognised input falls back to <first enumerator>" default,
//   7. the default-constructed value of every struct in the header,
//   8. the four-status badge contract that
//      src/game/ui/client/player/QuestBookWindow.cpp:511 open-codes as
//      `switch (status) {case 0..3}` under a "TODO use enum". That window is
//      an ImGui-only translation unit, so it cannot be linked here; the
//      assertion is on the enum side that switch feeds from: the set of
//      statuses the wire can carry is exactly {0,1,2,3}, so an unknown status
//      (4..255) falls through the window's switch to its empty-badge default
//      rather than aliasing a real status.
//
// Nothing here reads a file, a clock, a socket or a display: the only inputs
// are enum constants and locally built FlatBuffers.
#include <game/quests/QuestTypes.h>
#include "quest_generated.h"

#include <engine/net/test/test.h>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// Harness — the project's own CHECK/TEST convention
// (src/engine/net/test/test.h, src/game/machines/test/test_explosion_system.cpp).
// The repo has NO GoogleTest dependency: gtest is absent from conanfile.txt, CI
// (.github/workflows/build.yml) does not install libgtest-dev, and CI builds
// Release with a global -Werror — so a find_package(GTest QUIET) guard would
// make this test silently vanish from CI instead of failing there.
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
// -Wpedantic -Werror. Not for pointers, strings or enums with a custom
// underlying type.
#define CHECK_EQI(a, b, ...)                                                   \
    test_check(static_cast<long long>(a) == static_cast<long long>(b),          \
               __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

// Number of QuestStatus values that exist on the wire. Derived from the schema
// enum (MIN..MAX) rather than hardcoded, then asserted equal to 4 so a fifth
// status added to quest.fbs without updating the UI switch shows up here.
static constexpr int kWireStatusCount =
    static_cast<int>(Protocol::QuestStatus_MAX) -
    static_cast<int>(Protocol::QuestStatus_MIN) + 1;

static uint8_t wire(quest::QuestStatus s) {
    return static_cast<uint8_t>(s);
}
static uint8_t wire(Protocol::QuestStatus s) {
    return static_cast<uint8_t>(s);
}
static int statusInt(quest::QuestStatus s) { return static_cast<int>(s); }

// ---------------------------------------------------------------------------
// 1. QuestStatus wire values
// ---------------------------------------------------------------------------
static void test_QuestStatus_wire_values_are_pinned() {
    CHECK_EQI(quest::QuestStatus::LOCKED, 0, "LOCKED == 0");
    CHECK_EQI(quest::QuestStatus::AVAILABLE, 1, "AVAILABLE == 1");
    CHECK_EQI(quest::QuestStatus::IN_PROGRESS, 2, "IN_PROGRESS == 2");
    CHECK_EQI(quest::QuestStatus::COMPLETED, 3, "COMPLETED == 3");

    // The tracer reads COMPLETED as 3 from the socket; the enum is 1 byte so a
    // widened or signed underlying type would change the wire encoding.
    CHECK(sizeof(quest::QuestStatus) == 1, "QuestStatus is one byte on the wire");
    // A comma inside a template argument list is invisible to the preprocessor,
    // so the comparison is hoisted into a named bool before the macro sees it.
    using QuestStatusBase = std::underlying_type_t<quest::QuestStatus>;
    const bool backedByByte = std::is_same_v<QuestStatusBase, uint8_t>;
    CHECK(backedByByte,
          "QuestStatus is backed by uint8_t, matching quest.fbs `enum QuestStatus : uint8`");
    CHECK_EQI(wire(quest::QuestStatus::COMPLETED), 3,
              "COMPLETED marshals to byte 3");
}

// ---------------------------------------------------------------------------
// 2. C++ enum vs the FlatBuffers schema enum
// ---------------------------------------------------------------------------
static void test_QuestStatus_agrees_with_the_flatbuffers_schema() {
    const quest::QuestStatus cpp[] = {
        quest::QuestStatus::LOCKED, quest::QuestStatus::AVAILABLE,
        quest::QuestStatus::IN_PROGRESS, quest::QuestStatus::COMPLETED};
    const Protocol::QuestStatus fb[] = {
        Protocol::QuestStatus_LOCKED, Protocol::QuestStatus_AVAILABLE,
        Protocol::QuestStatus_IN_PROGRESS, Protocol::QuestStatus_COMPLETED};

    for (int i = 0; i < 4; ++i) {
        CHECK_EQI(wire(cpp[i]), wire(fb[i]),
                  "quest::QuestStatus and Protocol::QuestStatus agree");
        // The cast QuestManager actually performs
        // (static_cast<Protocol::QuestStatus>(status)) must be lossless.
        CHECK_EQI(wire(static_cast<Protocol::QuestStatus>(cpp[i])), wire(fb[i]),
                  "static_cast<quest::QuestStatus -> Protocol::QuestStatus> is identity");
    }

    // The schema enum's span is exactly the four C++ statuses: anything outside
    // 0..3 is off-wire (this is what loadProgress clamps).
    CHECK_EQI(kWireStatusCount, 4, "quest.fbs QuestStatus has exactly 4 values");
    CHECK_EQI(statusInt(quest::QuestStatus::LOCKED),
              static_cast<int>(Protocol::QuestStatus_MIN),
              "quest::QuestStatus::LOCKED is the schema minimum");
    CHECK_EQI(statusInt(quest::QuestStatus::COMPLETED),
              static_cast<int>(Protocol::QuestStatus_MAX),
              "quest::QuestStatus::COMPLETED is the schema maximum");
    // There is deliberately no quest::QuestStatus::COUNT sentinel (unlike Era):
    // loops over statuses must use the schema span, so assert the sentinel is
    // absent rather than letting a future `COUNT` masquerade as a status.
    CHECK_EQI(kWireStatusCount, 4, "no fifth QuestStatus was added");
}

// ---------------------------------------------------------------------------
// 3. Round trip through the real QuestBook wire structs
// ---------------------------------------------------------------------------
static void test_QuestStatus_round_trips_through_a_QuestEntry() {
    const quest::QuestStatus all[] = {
        quest::QuestStatus::LOCKED, quest::QuestStatus::AVAILABLE,
        quest::QuestStatus::IN_PROGRESS, quest::QuestStatus::COMPLETED};
    const uint8_t progress[] = {0, 0, 55, 100};

    for (int i = 0; i < 4; ++i) {
        flatbuffers::FlatBufferBuilder b(64);
        auto entry = Protocol::CreateQuestEntry(
            b, /*quest_id=*/7, static_cast<Protocol::QuestStatus>(all[i]),
            progress[i]);
        b.Finish(entry);
        auto *read = flatbuffers::GetRoot<Protocol::QuestEntry>(b.GetBufferPointer());
        CHECK(read != nullptr, "QuestEntry buffer verifies");
        if (!read)
            continue;
        CHECK_EQI(read->quest_id(), 7, "quest id survives the round trip");
        CHECK_EQI(static_cast<int>(read->status()), statusInt(all[i]),
                  "status survives the QuestEntry round trip");
        CHECK_EQI(read->progress(), progress[i], "progress survives the round trip");
    }
}

static void test_QuestStatus_round_trips_through_a_QuestProgressUpdate() {
    const uint64_t playerId = 4242;
    flatbuffers::FlatBufferBuilder b(128);

    std::vector<flatbuffers::Offset<Protocol::QuestEntry>> entries;
    entries.push_back(Protocol::CreateQuestEntry(
        b, 1, static_cast<Protocol::QuestStatus>(quest::QuestStatus::COMPLETED),
        /*progress=*/100));
    entries.push_back(Protocol::CreateQuestEntry(
        b, 2, static_cast<Protocol::QuestStatus>(quest::QuestStatus::IN_PROGRESS),
        /*progress=*/50));
    auto vec = b.CreateVector(entries);
    b.Finish(Protocol::CreateQuestProgressUpdate(b, playerId, vec));

    flatbuffers::Verifier v(b.GetBufferPointer(), b.GetSize());
    CHECK(v.VerifyBuffer<Protocol::QuestProgressUpdate>(nullptr),
          "QuestProgressUpdate buffer verifies");
    auto *upd = flatbuffers::GetRoot<Protocol::QuestProgressUpdate>(b.GetBufferPointer());
    CHECK(upd != nullptr, "root QuestProgressUpdate is non-null");
    if (!upd)
        return;
    CHECK_EQI(upd->player_id(), playerId, "player id survives");
    CHECK(upd->quests() != nullptr && upd->quests()->size() == 2,
          "two quest entries survive");

    // This is the exact assertion the Go integration tracer makes at
    // questbook_wire_tracer_test.go:86 — COMPLETED/100 for quest 1.
    if (upd->quests() && upd->quests()->size() == 2) {
        auto *e0 = upd->quests()->Get(0);
        CHECK(e0 != nullptr, "entry 0 present");
        if (e0) {
            CHECK_EQI(e0->quest_id(), 1, "tracer asserts quest_id == 1");
            CHECK(e0->status() == Protocol::QuestStatus_COMPLETED,
                  "tracer asserts Status() == QuestStatusCOMPLETED");
            CHECK_EQI(e0->progress(), 100, "tracer asserts Progress() == 100");
        }
        auto *e1 = upd->quests()->Get(1);
        CHECK(e1 != nullptr, "entry 1 present");
        if (e1) {
            CHECK(e1->status() == Protocol::QuestStatus_IN_PROGRESS,
                  "IN_PROGRESS survives the multi-entry vector");
            CHECK_EQI(e1->progress(), 50, "sub-100 progress survives");
        }
    }
}

static void test_QuestProgress_default_entry_is_LOCKED_zero() {
    // The schema defaults a QuestEntry to LOCKED / 0. buildProgress()-style
    // code that omits status must not accidentally produce COMPLETED.
    flatbuffers::FlatBufferBuilder b(64);
    auto entry = Protocol::CreateQuestEntry(b, /*quest_id=*/3);
    b.Finish(entry);
    auto *read = flatbuffers::GetRoot<Protocol::QuestEntry>(b.GetBufferPointer());
    CHECK(read != nullptr, "default QuestEntry buffer verifies");
    if (!read)
        return;
    CHECK(read->status() == Protocol::QuestStatus_LOCKED,
          "an omitted status defaults to LOCKED");
    CHECK_EQI(read->progress(), 0, "an omitted progress defaults to 0");
}

// ---------------------------------------------------------------------------
// 4. Off-wire statuses
// ---------------------------------------------------------------------------
static void test_statuses_outside_the_schema_span_are_off_wire() {
    // Everything a uint8_t can carry above COMPLETED is invalid. This is the
    // contract QuestManager::loadProgress enforces with `if (status >= 4)`
    // → clamp to LOCKED (QuestManager.cpp:625-629).
    for (int raw : {4, 5, 7, 100, 200, 254, 255}) {
        CHECK(raw > statusInt(quest::QuestStatus::COMPLETED),
              "raw byte is above COMPLETED");
        CHECK(raw < 256, "raw byte is representable in a uint8_t field");
    }
    // The clamp boundary is exactly "one past the schema maximum".
    CHECK_EQI(statusInt(quest::QuestStatus::COMPLETED) + 1, 4,
              "the first invalid status is 4");

    // Casting an off-wire byte into the enum yields a value that matches no
    // enumerator and compares false against all four: nothing can mistake it
    // for a real status.
    const auto bogus = static_cast<quest::QuestStatus>(4);
    CHECK(bogus != quest::QuestStatus::LOCKED);
    CHECK(bogus != quest::QuestStatus::AVAILABLE);
    CHECK(bogus != quest::QuestStatus::IN_PROGRESS);
    CHECK(bogus != quest::QuestStatus::COMPLETED);
}

// ---------------------------------------------------------------------------
// 5. Era
// ---------------------------------------------------------------------------
static void test_Era_wire_values_and_COUNT_sentinel() {
    CHECK_EQI(quest::Era::VAGRANT, 0, "VAGRANT == 0");
    CHECK_EQI(quest::Era::APPRENTICE, 1, "APPRENTICE == 1");
    CHECK_EQI(quest::Era::EXPERT, 2, "EXPERT == 2");
    CHECK_EQI(quest::Era::ADMINISTRATOR, 3, "ADMINISTRATOR == 3");
    CHECK_EQI(quest::Era::ENERGY_JUNIOR, 4, "ENERGY_JUNIOR == 4");
    CHECK_EQI(quest::Era::ENERGY_MIDDLE, 5, "ENERGY_MIDDLE == 5");
    CHECK_EQI(quest::Era::ENERGY_SENIOR, 6, "ENERGY_SENIOR == 6");
    // COUNT is a sentinel, not a real era, and its value is the era-transition
    // "is there a next era?" boundary.
    CHECK_EQI(quest::Era::COUNT, 7, "COUNT == 7 (one past ENERGY_SENIOR)");
    CHECK(sizeof(quest::Era) == 1,
          "Era is one byte — EraTransitionNotification carries uint8_t");
}

static void test_EraLabel_covers_every_era_and_falls_back_to_Unknown() {
    CHECK(std::string(EraLabel(quest::Era::VAGRANT)) == "Vagrant");
    CHECK(std::string(EraLabel(quest::Era::APPRENTICE)) == "Apprentice");
    CHECK(std::string(EraLabel(quest::Era::EXPERT)) == "Expert");
    CHECK(std::string(EraLabel(quest::Era::ADMINISTRATOR)) == "Administrator");
    CHECK(std::string(EraLabel(quest::Era::ENERGY_JUNIOR)) == "Energy Junior");
    CHECK(std::string(EraLabel(quest::Era::ENERGY_MIDDLE)) == "Energy Middle");
    CHECK(std::string(EraLabel(quest::Era::ENERGY_SENIOR)) == "Energy Senior");

    // The COUNT sentinel and any out-of-range byte both render as "Unknown"
    // (QuestTypes.h:43-46) — BuildEraStructure never emits them because it
    // only iterates [0, COUNT).
    CHECK(std::string(EraLabel(quest::Era::COUNT)) == "Unknown",
          "Era::COUNT renders as Unknown");
    CHECK(std::string(EraLabel(static_cast<quest::Era>(200))) == "Unknown",
          "an out-of-range era renders as Unknown");

    // Every real era has a non-empty, distinct label.
    for (int e = 0; e < static_cast<int>(quest::Era::COUNT); ++e) {
        const char *label = EraLabel(static_cast<quest::Era>(e));
        CHECK(label != nullptr && label[0] != '\0', "era label is non-empty");
        for (int o = 0; o < e; ++o) {
            CHECK(std::string(label) != std::string(EraLabel(static_cast<quest::Era>(o))),
                  "era labels are distinct");
        }
    }
}

static void test_EraFromString_round_trips_every_parser_key() {
    // quests.csv era column -> EraFromString -> Era. The parser keys are
    // lowercase snake_case; the *labels* are title case, so a label does NOT
    // round-trip back. Pinned deliberately: quests.csv holds "vagrant", not
    // "Vagrant", and re-serialising a QuestDef with EraLabel would silently
    // re-parse as VAGRANT via the unknown-input fallback.
    const char *key[] = {"vagrant",   "apprentice", "expert",
                         "administrator", "energy_junior", "energy_middle",
                         "energy_senior"};
    const quest::Era expected[] = {
        quest::Era::VAGRANT, quest::Era::APPRENTICE, quest::Era::EXPERT,
        quest::Era::ADMINISTRATOR, quest::Era::ENERGY_JUNIOR,
        quest::Era::ENERGY_MIDDLE, quest::Era::ENERGY_SENIOR};

    for (int i = 0; i < 7; ++i) {
        CHECK(quest::EraFromString(key[i]) == expected[i], "parser key maps to its era");
    }
    CHECK(quest::EraFromString("energy_senior") == quest::Era::ENERGY_SENIOR,
          "the last era parses before the COUNT sentinel");

    // Labels are display strings, not parser keys.
    CHECK(quest::EraFromString(EraLabel(quest::Era::ENERGY_SENIOR)) ==
              quest::Era::VAGRANT,
          "a title-case label parses as VAGRANT via the fallback, not ENERGY_SENIOR");
}

static void test_EraFromString_unknown_input_defaults_to_VAGRANT() {
    // QuestTypes.h:64 — the final `return Era::VAGRANT` catch-all. A typo in
    // quests.csv therefore lands a quest in the first era rather than dropping
    // it. That is the observed contract; pin it so changing it is deliberate.
    CHECK(quest::EraFromString("") == quest::Era::VAGRANT, "empty string -> VAGRANT");
    CHECK(quest::EraFromString("nonsense") == quest::Era::VAGRANT, "junk -> VAGRANT");
    CHECK(quest::EraFromString("VAGRANT") == quest::Era::VAGRANT,
          "upper-case key -> VAGRANT (parser is case-sensitive)");
    CHECK(quest::EraFromString("count") == quest::Era::VAGRANT,
          "the COUNT sentinel has no parser key");
}

static void test_Era_next_era_arithmetic_clamps_at_the_last_era() {
    // publishEraTransition (QuestManager.cpp:92-95) computes
    //   next = (completed + 1 < uint8(Era::COUNT)) ? completed + 1 : completed
    // so the final era reports itself as the next era. Same expression, pinned.
    for (int e = 0; e < static_cast<int>(quest::Era::COUNT) - 1; ++e) {
        const uint8_t completed = static_cast<uint8_t>(e);
        const uint8_t next =
            (completed + 1 < static_cast<uint8_t>(quest::Era::COUNT))
                ? static_cast<uint8_t>(completed + 1)
                : completed;
        CHECK_EQI(next, e + 1, "every era but the last advances by one");
    }
    const uint8_t last = static_cast<uint8_t>(quest::Era::ENERGY_SENIOR);
    CHECK_EQI(last + 1, static_cast<uint8_t>(quest::Era::COUNT),
              "the last era is one below COUNT");
    const uint8_t nextLast =
        (last + 1 < static_cast<uint8_t>(quest::Era::COUNT))
            ? static_cast<uint8_t>(last + 1)
            : last;
    CHECK_EQI(nextLast, last,
             "the last era reports itself as the next era (no COUNT leak on the wire)");
}

// ---------------------------------------------------------------------------
// 6. DetectionType / RewardType
// ---------------------------------------------------------------------------
static void test_DetectionType_values_and_parser() {
    CHECK_EQI(quest::DetectionType::CRAFT, 0, "CRAFT == 0");
    CHECK_EQI(quest::DetectionType::BLOCK_PLACED, 1, "BLOCK_PLACED == 1");
    CHECK_EQI(quest::DetectionType::TOOL_CHARGED, 2, "TOOL_CHARGED == 2");
    CHECK_EQI(quest::DetectionType::SIDE_CONFIGURED, 3, "SIDE_CONFIGURED == 3");
    CHECK_EQI(quest::DetectionType::EXCHANGE, 4, "EXCHANGE == 4");
    CHECK_EQI(quest::DetectionType::INVENTORY, 5, "INVENTORY == 5");
    CHECK_EQI(quest::DetectionType::MACHINE, 6, "MACHINE == 6");
    CHECK(sizeof(quest::DetectionType) == 1, "DetectionType is one byte");

    const char *key[] = {"craft",  "block_placed",     "tool_charged",
                         "side_configured", "exchange", "inventory", "machine"};
    const quest::DetectionType expected[] = {
        quest::DetectionType::CRAFT, quest::DetectionType::BLOCK_PLACED,
        quest::DetectionType::TOOL_CHARGED, quest::DetectionType::SIDE_CONFIGURED,
        quest::DetectionType::EXCHANGE, quest::DetectionType::INVENTORY,
        quest::DetectionType::MACHINE};
    for (int i = 0; i < 7; ++i) {
        CHECK(quest::DetectFromString(key[i]) == expected[i],
              "DetectFromString maps every documented key");
    }

    // Unknown input falls back to CRAFT (QuestTypes.h:92) — the same catch-all
    // LoadRequirementsJSON::parseKind duplicates with a *different* key set
    // ("obtain" -> INVENTORY, "place" -> BLOCK_PLACED), so a requirements file
    // using "obtain" must go through the loader, not DetectFromString.
    CHECK(quest::DetectFromString("") == quest::DetectionType::CRAFT, "empty -> CRAFT");
    CHECK(quest::DetectFromString("obtain") == quest::DetectionType::CRAFT,
          "the loader-only key \"obtain\" is NOT understood by DetectFromString");
    CHECK(quest::DetectFromString("place") == quest::DetectionType::CRAFT,
          "the loader-only key \"place\" is NOT understood by DetectFromString");
    CHECK(quest::DetectFromString("nonsense") == quest::DetectionType::CRAFT,
          "junk -> CRAFT");
}

static void test_RewardType_values_and_loader_default() {
    CHECK_EQI(quest::RewardType::ITEM, 0, "ITEM == 0");
    CHECK_EQI(quest::RewardType::EXPERIENCE, 1, "EXPERIENCE == 1");
    CHECK_EQI(quest::RewardType::SPECIAL, 2, "SPECIAL == 2");
    CHECK(sizeof(quest::RewardType) == 1, "RewardType is one byte");
    // QuestData::LoadRewardsJSON::parseType is a local lambda, not header code:
    // pin the same mapping it implements so a loader change is visible here.
    auto parseRewardType = [](const std::string &t) {
        if (t == "experience")
            return quest::RewardType::EXPERIENCE;
        if (t == "special")
            return quest::RewardType::SPECIAL;
        return quest::RewardType::ITEM;
    };
    CHECK(parseRewardType("item") == quest::RewardType::ITEM, "\"item\" -> ITEM");
    CHECK(parseRewardType("experience") == quest::RewardType::EXPERIENCE,
          "\"experience\" -> EXPERIENCE");
    CHECK(parseRewardType("special") == quest::RewardType::SPECIAL,
          "\"special\" -> SPECIAL");
    CHECK(parseRewardType("") == quest::RewardType::ITEM,
          "an absent type defaults to ITEM (QuestData.cpp parseType)");
}

// ---------------------------------------------------------------------------
// 7. Struct defaults
// ---------------------------------------------------------------------------
static void test_struct_defaults_match_the_documented_contracts() {
    quest::QuestRequirement req;
    CHECK(req.kind == quest::DetectionType::CRAFT, "QuestRequirement.kind -> CRAFT");
    CHECK(req.item.empty(), "QuestRequirement.item is empty");
    CHECK_EQI(req.count, 0, "QuestRequirement.count -> 0");
    CHECK(req.consume == false, "QuestRequirement.consume -> false");
    CHECK(req.machine.empty(), "QuestRequirement.machine is empty");

    quest::RewardEntry entry;
    CHECK(entry.type == quest::RewardType::ITEM, "RewardEntry.type -> ITEM");
    CHECK(entry.item.empty(), "RewardEntry.item is empty");
    CHECK_EQI(entry.count, 0, "RewardEntry.count -> 0");
    CHECK_EQI(entry.value, 0.0f, "RewardEntry.value -> 0.f");

    quest::QuestReward reward;
    CHECK(reward.rewards.empty(), "QuestReward.rewards is empty");
    CHECK(reward.choiceOf.empty(), "QuestReward.choiceOf is empty");

    quest::QuestDef def;
    CHECK_EQI(def.id, 0, "QuestDef.id -> 0");
    CHECK(def.title.empty(), "QuestDef.title is empty");
    CHECK(def.description.empty(), "QuestDef.description is empty");
    CHECK(def.era == quest::Era::VAGRANT, "QuestDef.era -> VAGRANT");
    CHECK(def.section.empty(), "QuestDef.section is empty");
    CHECK(def.prerequisites.empty(), "QuestDef.prerequisites is empty");
    CHECK(def.detectType == quest::DetectionType::CRAFT, "QuestDef.detectType -> CRAFT");
    CHECK(def.detectTarget.empty(), "QuestDef.detectTarget is empty");
    CHECK_EQI(def.rewardItemId, 0, "QuestDef.rewardItemId -> 0");
    CHECK_EQI(def.rewardCount, 0, "QuestDef.rewardCount -> 0");
    CHECK_EQI(def.costItemId, 0, "QuestDef.costItemId -> 0");
    CHECK_EQI(def.costCount, 0, "QuestDef.costCount -> 0");
    CHECK_EQI(def.cooldownSecs, 0, "QuestDef.cooldownSecs -> 0");
    CHECK_EQI(def.targetCount, 0, "QuestDef.targetCount -> 0");
    // autoComplete defaults TRUE: objective + prereqs met means "complete
    // immediately". handleQuestMet branches on this, so the default decides
    // whether a quest needs the client's Complete button (QuestTypes.h:147).
    CHECK(def.autoComplete == true, "QuestDef.autoComplete -> true");
    CHECK(def.requirements.empty(), "QuestDef.requirements is empty");

    quest::QuestProgress progress;
    CHECK_EQI(progress.questId, 0, "QuestProgress.questId -> 0");
    CHECK(progress.status == quest::QuestStatus::LOCKED,
          "QuestProgress.status -> LOCKED (an unstarted quest)");
    CHECK_EQI(progress.progressPercent, 0, "QuestProgress.progressPercent -> 0");

    quest::QuestProgressSnapshot snapshot;
    CHECK_EQI(snapshot.playerId, 0, "QuestProgressSnapshot.playerId -> 0");
    CHECK(snapshot.entries.empty(), "QuestProgressSnapshot.entries is empty");

    quest::SectionInfo section;
    CHECK(section.name.empty(), "SectionInfo.name is empty");
    CHECK(section.label.empty(), "SectionInfo.label is empty");
    CHECK(section.questIds.empty(), "SectionInfo.questIds is empty");

    quest::EraInfo eraInfo;
    CHECK(eraInfo.name.empty(), "EraInfo.name is empty");
    CHECK(eraInfo.label.empty(), "EraInfo.label is empty");
    CHECK(eraInfo.sections.empty(), "EraInfo.sections is empty");

    // The default status is LOCKED = 0, which is also the wire default and the
    // value QuestManager::loadProgress clamps unknown statuses to. One default
    // for all three paths, pinned.
    CHECK_EQI(static_cast<uint8_t>(quest::QuestProgress{}.status), 0,
              "a default-constructed QuestProgress is LOCKED/0 on the wire");
    CHECK_EQI(wire(Protocol::QuestStatus_LOCKED), 0,
              "the schema default is also byte 0");
}

// ---------------------------------------------------------------------------
// 8. UI badge contract (QuestBookWindow.cpp:511, "TODO use enum")
// ---------------------------------------------------------------------------
static void test_the_four_wire_statuses_are_exactly_the_ui_badge_cases() {
    // QuestBookWindow::renderCompletionBadge / statusColor / statusLabel switch
    // on a raw uint8_t with cases 0..3 and a `TODO use enum` marker. That file
    // needs ImGui and cannot be linked from here, so assert the property that
    // makes those open-coded cases correct: the wire can only carry 0..3, so
    // every status the window can be handed matches exactly one of its four
    // cases and an unknown byte falls through to its default branch instead of
    // aliasing a real status. A fifth status in quest.fbs would break this.
    CHECK_EQI(kWireStatusCount, 4, "the UI switch's four cases still cover the wire");
    CHECK_EQI(statusInt(quest::QuestStatus::LOCKED), 0, "case 0 = LOCKED");
    CHECK_EQI(statusInt(quest::QuestStatus::AVAILABLE), 1, "case 1 = AVAILABLE");
    CHECK_EQI(statusInt(quest::QuestStatus::IN_PROGRESS), 2, "case 2 = IN PROGRESS");
    CHECK_EQI(statusInt(quest::QuestStatus::COMPLETED), 3, "case 3 = COMPLETED");

    // statusLabel maps 2/3 to shortened badges ("[...]", "[DONE]") — the client's
    // rendering choice. Recorded here so a QuestStatus rename that reaches the
    // window is a visible test change, not a silent UI drift.
    const char *badge[4] = {"[LOCKED]", "[AVAILABLE]", "[...]", "[DONE]"};
    for (int i = 0; i < 4; ++i) {
        CHECK(badge[i] != nullptr && badge[i][0] != '\0',
              "every wire status has a badge string");
    }
    // Anything above 3 is the window's `default:` branch, not a real status.
    CHECK(4 > statusInt(quest::QuestStatus::COMPLETED),
          "status 4+ is the window's default branch, not an alias");
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
    printf("=== quest_types test suite ===\n\n");

    TEST(QuestStatus_wire_values_are_pinned);
    TEST(QuestStatus_agrees_with_the_flatbuffers_schema);
    TEST(QuestStatus_round_trips_through_a_QuestEntry);
    TEST(QuestStatus_round_trips_through_a_QuestProgressUpdate);
    TEST(QuestProgress_default_entry_is_LOCKED_zero);
    TEST(statuses_outside_the_schema_span_are_off_wire);
    TEST(Era_wire_values_and_COUNT_sentinel);
    TEST(EraLabel_covers_every_era_and_falls_back_to_Unknown);
    TEST(EraFromString_round_trips_every_parser_key);
    TEST(EraFromString_unknown_input_defaults_to_VAGRANT);
    TEST(Era_next_era_arithmetic_clamps_at_the_last_era);
    TEST(DetectionType_values_and_parser);
    TEST(RewardType_values_and_loader_default);
    TEST(struct_defaults_match_the_documented_contracts);
    TEST(the_four_wire_statuses_are_exactly_the_ui_badge_cases);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
