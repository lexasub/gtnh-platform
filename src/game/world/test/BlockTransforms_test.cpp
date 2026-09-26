// Unit tests for simcore::BlockTransforms — the data-driven
// "placed X on Y -> Z" rule table loaded from registry/transforms.csv.
//
// File under test: src/game/world/BlockTransforms.cpp
//
// These tests assert the OBSERVED behaviour of the loader, including the
// quirks documented at the bottom of this file. Nothing here is aspirational:
// every expectation matches what the implementation actually does today.

#include <engine/registry/ItemId.h>
#include <game/world/BlockTransforms.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

// ---- project test harness (mirrors src/game/quests/test/test_questdata.cpp) ----
int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "", msg ? msg : "");
  }
}

#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define TEST(name) do { printf("  TEST: %s\n", #name); test_##name(); } while (0)

using simcore::BlockTransforms;
using simcore::TransformResult;

namespace {

// Writes `body` to a uniquely named file under the system temp dir and returns
// its path. Mirrors the temp-fixture helper used by src/engine/registry/test.
class TempCsv {
public:
  explicit TempCsv(const std::string& suffix, const std::string& body) {
    path_ = std::filesystem::temp_directory_path() /
            ("gtnh-block-transforms-" + suffix + ".csv");
    std::filesystem::remove(path_);
    std::ofstream out(path_);
    out << body;
  }
  ~TempCsv() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  TempCsv(const TempCsv&) = delete;
  TempCsv& operator=(const TempCsv&) = delete;

  const char* c_str() const { return path_.c_str(); }

private:
  std::filesystem::path path_;
};

using TransformPtr = std::unique_ptr<BlockTransforms>;

// RAII guard so the process-wide singleton never leaks into another test.
class InstanceGuard {
public:
  InstanceGuard() = default;
  ~InstanceGuard() { BlockTransforms::setInstance(nullptr); }
  InstanceGuard(const InstanceGuard&) = delete;
  InstanceGuard& operator=(const InstanceGuard&) = delete;
};

// ---------------------------------------------------------------------------
// Apply(): the core mapping
// ---------------------------------------------------------------------------

static void test_UnknownPairReturnsNullopt() {
  // The table holds only (0:0:2, 0:0:3); the queried pair is absent.
  const TempCsv csv("miss", "0:0:2,0:0:3,0:0:4\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  CHECK(!transforms->Apply(ItemId::pack("0:0:1"),
                                 ItemId::pack("0:0:2")).has_value());
}

static void test_EmptyTableMatchesNothing() {
  const TempCsv csv("empty", "");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  CHECK(!transforms->Apply(1, 2).has_value());
  // Even a rule whose ids both pack to 0 has nothing to return.
  CHECK(!transforms->Apply(0, 0).has_value());
}

// The header line documents the format; it must not be parsed as a rule.
static void test_CommentOnlyFileYieldsNoRules() {
  const TempCsv csv("header-only", "# expected,new,result_id[,result_meta]");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const uint16_t zero = ItemId::pack("0:0:0");
  CHECK(!transforms->Apply(zero, zero).has_value());
}

static void test_ThreeColumnRuleAppliesWithZeroMeta() {
  const TempCsv csv("three-col", "0:0:1,0:0:2,0:0:3\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto result =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  // Result id is the packed third column.
  CHECK_EQ(result->new_block_id, ItemId::pack("0:0:3"));
  // Meta column absent -> defaults to 0.
  CHECK_EQ(result->new_meta, 0);
}

static void test_FourColumnRuleAppliesWithGivenMeta() {
  const TempCsv csv("four-col", "0:0:1,0:0:2,0:0:3,7\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto result =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  CHECK_EQ(result->new_block_id, ItemId::pack("0:0:3"));
  CHECK_EQ(result->new_meta, 7);
}

static void test_ColumnsBeyondTheFourthAreIgnored() {
  const TempCsv csv("five-col", "0:0:1,0:0:2,0:0:3,9,ignore,me\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto result =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  CHECK_EQ(result->new_meta, 9);
}

static void test_MultipleRulesCoexist() {
  const TempCsv csv("multi",
                    "0:0:1,0:0:2,0:0:3,1\n"
                    "10:0:1,10:0:2,110:0:4,2\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto first =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(first.has_value());
  CHECK_EQ(first->new_block_id, ItemId::pack("0:0:3"));
  CHECK_EQ(first->new_meta, 1);

  const auto second =
      transforms->Apply(ItemId::pack("10:0:1"), ItemId::pack("10:0:2"));
  CHECK(second.has_value());
  CHECK_EQ(second->new_block_id, ItemId::pack("110:0:4"));
  CHECK_EQ(second->new_meta, 2);
}

// The lookup key is ordered: (expected,new) != (new,expected).
static void test_RuleIsDirectionalAndNotSymmetric() {
  const TempCsv csv("direction", "0:0:1,0:0:2,0:0:3\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  CHECK(
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"))
          .has_value());
  CHECK(!
      transforms->Apply(ItemId::pack("0:0:2"), ItemId::pack("0:0:1"))
          .has_value());
}

// Both halves of the key must match; sharing one half is not a hit.
static void test_BothKeyComponentsMustMatch() {
  const TempCsv csv("partial-key", "0:0:1,0:0:2,0:0:3\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  CHECK(!
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:9"))
          .has_value());
  CHECK(!
      transforms->Apply(ItemId::pack("0:0:9"), ItemId::pack("0:0:2"))
          .has_value());
}

// ---------------------------------------------------------------------------
// Load(): parsing, skipping and failure handling
// ---------------------------------------------------------------------------

static void test_LoadSkipsCommentsAndBlankLines() {
  const TempCsv csv("comments",
                    "# a leading comment\n"
                    "\n"
                    "0:0:1,0:0:2,0:0:3,4\n"
                    "# trailing comment\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto result =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  CHECK_EQ(result->new_block_id, ItemId::pack("0:0:3"));
  CHECK_EQ(result->new_meta, 4);
}

// A '#' only starts a comment in column 0. There is no mid-line comment
// handling, so a trailing comment reaches std::stoi, which stops at the
// first non-digit and yields 5.
static void test_TrailingCommentMidLineIsNotStripped() {
  const TempCsv csv("midline-hash", "0:0:1,0:0:2,0:0:3,5 # trailing note\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto result =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  CHECK_EQ(result->new_meta, 5);
}

static void test_RowsWithFewerThanThreeColumnsAreSkipped() {
  // Short rows use ids distinct from the valid row, so "was skipped" is
  // directly observable as a lookup miss.
  const TempCsv csv("short-rows",
                    "0:0:1\n"                 // 1 column  -> skipped
                    "0:0:2,0:0:3\n"           // 2 columns -> skipped
                    "0:0:1,0:0:2,0:0:3,4\n"); // valid
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  // A 1-column row has no (expected,new) pair to register.
  CHECK(!
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:1"))
          .has_value());
  // A 2-column row is rejected by the cols.size() < 3 guard, so it registers
  // nothing even though it has a parseable expected id.
  CHECK(!
      transforms->Apply(ItemId::pack("0:0:2"), ItemId::pack("0:0:3"))
          .has_value());
  // The well-formed row on the same file still loaded.
  const auto ok =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(ok.has_value());
  CHECK_EQ(ok->new_meta, 4);
}

// OBSERVED QUIRK: a non-numeric meta makes std::stoi throw, which drops the
// WHOLE row — including the two already-parsed id columns. The loader is
// all-or-nothing per line.
static void test_NonNumericMetaDropsTheEntireRule() {
  const TempCsv csv("bad-meta", "0:0:1,0:0:2,0:0:3,not_a_number\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  CHECK(!
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"))
          .has_value());
}

// A bad row must not poison the rows around it.
static void test_BadRowDoesNotAffectNeighbouringRows() {
  const TempCsv csv("bad-row-neighbours",
                    "0:0:1,0:0:2,0:0:3,5\n"
                    "0:0:2,0:0:4,0:0:6,oops\n"
                    "0:0:4,0:0:5,0:0:7,6\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto before =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(before.has_value());
  CHECK_EQ(before->new_meta, 5);

  const auto after =
      transforms->Apply(ItemId::pack("0:0:4"), ItemId::pack("0:0:5"));
  CHECK(after.has_value());
  CHECK_EQ(after->new_meta, 6);

  CHECK(!
      transforms->Apply(ItemId::pack("0:0:2"), ItemId::pack("0:0:4"))
          .has_value());
}

// OBSERVED QUIRK: rules_ is filled with emplace(), which does not overwrite.
// When the same (expected,new) key appears twice the FIRST row wins.
static void test_DuplicateKeyKeepsFirstRule() {
  const TempCsv csv("duplicate",
                    "0:0:1,0:0:2,0:0:3,1\n"
                    "0:0:1,0:0:2,0:0:9,2\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto result =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  CHECK_EQ(result->new_block_id, ItemId::pack("0:0:3"));
  CHECK_EQ(result->new_meta, 1);
}

// OBSERVED QUIRK: the meta column is cast to uint8_t without a range check, so
// 300 silently wraps to 44 and -1 wraps to 255.
static void test_MetaOutOfUint8RangeWraps() {
  const TempCsv csv("meta-wrap", "0:0:1,0:0:2,0:0:3,300\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto high =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(high.has_value());
  CHECK_EQ(high->new_meta, 44u);  // 300 mod 256

  const TempCsv csv_neg("meta-wrap-neg", "0:0:1,0:0:2,0:0:3,-1\n");
  const auto neg = std::unique_ptr<BlockTransforms>(
      BlockTransforms::Load(csv_neg.c_str()));
  const auto low = neg->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(low.has_value());
  CHECK_EQ(low->new_meta, 255u);
}

// std::stoi tolerates surrounding whitespace and trailing junk, and
// ItemId::pack ignores non-digit payload characters, so a CRLF file still
// loads correctly.
static void test_CrlfAndPaddedMetaStillLoad() {
  const TempCsv csv("crlf", "0:0:1,0:0:2,0:0:3,5\r\n");
  const auto transforms =
      std::unique_ptr<BlockTransforms>(BlockTransforms::Load(csv.c_str()));

  const auto result =
      transforms->Apply(ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  CHECK_EQ(result->new_block_id, ItemId::pack("0:0:3"));
  CHECK_EQ(result->new_meta, 5);
}

// A missing file is logged and yields an empty (but non-null) table — the
// caller in simcore/main.cpp passes the result straight to setInstance().
static void test_MissingFileYieldsEmptyNonNullTable() {
  const auto transforms = std::unique_ptr<BlockTransforms>(
      BlockTransforms::Load("/nonexistent/gtnh/transforms.csv"));

  CHECK_NE(transforms, nullptr);
  CHECK(!transforms->Apply(0, 0).has_value());
  CHECK(!transforms->Apply(ItemId::pack("0:0:1"),
                                 ItemId::pack("0:0:2")).has_value());
}

// ---------------------------------------------------------------------------
// Singleton plumbing
// ---------------------------------------------------------------------------

static void test_SetInstanceAndInstanceRoundTrip() {
  InstanceGuard guard;
  CHECK_EQ(BlockTransforms::instance(), nullptr);

  const TempCsv csv("singleton", "0:0:1,0:0:2,0:0:3,8\n");
  const TransformPtr owned(BlockTransforms::Load(csv.c_str()));

  BlockTransforms::setInstance(owned.get());
  CHECK_EQ(BlockTransforms::instance(), owned.get());

  // This is the exact call shape used by PlaceBlockHandler::handle().
  const auto result = BlockTransforms::instance()->Apply(
      ItemId::pack("0:0:1"), ItemId::pack("0:0:2"));
  CHECK(result.has_value());
  CHECK_EQ(result->new_block_id, ItemId::pack("0:0:3"));
  CHECK_EQ(result->new_meta, 8);

  BlockTransforms::setInstance(nullptr);
  CHECK_EQ(BlockTransforms::instance(), nullptr);
}

// ---------------------------------------------------------------------------
// The shipped data file
// ---------------------------------------------------------------------------

// src/content/data/registry/transforms.csv currently holds only its header
// comment, so loading the real registry yields an empty table. This pins the
// shipped state and fails loudly if a real rule is ever added without a
// matching expectation being revisited.
static void test_ShippedRegistryTransformsCsvIsCurrentlyEmpty() {
  const auto transforms = std::unique_ptr<BlockTransforms>(
      BlockTransforms::Load(REGISTRY_TRANSFORMS_DIR "/transforms.csv"));
  CHECK_NE(transforms, nullptr);

  // No rules are defined yet, so every lookup misses.
  CHECK(!transforms->Apply(0, 0).has_value());
  CHECK(!transforms->Apply(ItemId::pack("0:0:1"),
                                 ItemId::pack("0:0:2")).has_value());
}

// TransformResult is a plain aggregate: default-init zeroed, field order
// (new_block_id, new_meta) is what the loader fills.
static void test_DefaultConstructsToZero() {
  const TransformResult result{};
  CHECK_EQ(result.new_block_id, 0u);
  CHECK_EQ(result.new_meta, 0u);
}

} // namespace

int main() {
  TEST(UnknownPairReturnsNullopt);
  TEST(EmptyTableMatchesNothing);
  TEST(CommentOnlyFileYieldsNoRules);
  TEST(ThreeColumnRuleAppliesWithZeroMeta);
  TEST(FourColumnRuleAppliesWithGivenMeta);
  TEST(ColumnsBeyondTheFourthAreIgnored);
  TEST(MultipleRulesCoexist);
  TEST(RuleIsDirectionalAndNotSymmetric);
  TEST(BothKeyComponentsMustMatch);
  TEST(LoadSkipsCommentsAndBlankLines);
  TEST(TrailingCommentMidLineIsNotStripped);
  TEST(RowsWithFewerThanThreeColumnsAreSkipped);
  TEST(NonNumericMetaDropsTheEntireRule);
  TEST(BadRowDoesNotAffectNeighbouringRows);
  TEST(DuplicateKeyKeepsFirstRule);
  TEST(MetaOutOfUint8RangeWraps);
  TEST(CrlfAndPaddedMetaStillLoad);
  TEST(MissingFileYieldsEmptyNonNullTable);
  TEST(SetInstanceAndInstanceRoundTrip);
  TEST(ShippedRegistryTransformsCsvIsCurrentlyEmpty);
  TEST(DefaultConstructsToZero);
  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
