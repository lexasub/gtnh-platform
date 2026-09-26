// Unit tests for simcore::BlockDrops — the data-driven "breaking block X
// yields item Y" rule table loaded from registry/drops.csv.
//
// File under test: src/game/world/BlockDrops.cpp
//
// These tests assert the OBSERVED behaviour of the loader, including the
// quirks documented at the bottom of this file. Nothing here is aspirational:
// every expectation matches what the implementation actually does today.
//
// The issue (gp-fmeu) asked for two things: the drop table lookup, and that
// an unknown block yields the documented empty result. Both are pinned here,
// plus the loader's validation behaviour, which turned out to be weaker than
// the "bad line" warning suggests — see the QUIRKS section.

// ---- project test harness (mirrors src/game/world/test/BlockTransforms_test.cpp) ----
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg = nullptr) {
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

#include <engine/registry/ItemId.h>
#include <game/world/BlockDrops.h>

using simcore::BlockDrops;
using simcore::DropInfo;

namespace {

// Writes `body` to a uniquely named file under the system temp dir and returns
// its path. Mirrors the TempCsv helper in BlockTransforms_test.cpp.
class TempCsv {
public:
  explicit TempCsv(const std::string& suffix, const std::string& body) {
    path_ = std::filesystem::temp_directory_path() /
            ("gtnh-block-drops-" + suffix + ".csv");
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

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

// RAII guard so the process-wide BlockDrops singleton never leaks into another
// test (same guard as src/game/actions/test/test_action_dispatch.cpp).
class DropsGuard {
public:
  DropsGuard() { BlockDrops::setInstance(nullptr); }
  ~DropsGuard() { BlockDrops::setInstance(nullptr); }
  DropsGuard(const DropsGuard&) = delete;
  DropsGuard& operator=(const DropsGuard&) = delete;
};

// Writes `body` to the temp file named after `tag`, loads it, and returns the
// table. The file is fully read into the map inside Load, so the temp file is
// removed again before this returns.
BlockDrops* loadCsv(const std::string& tag, const std::string& body) {
  TempCsv csv(tag, body);
  return BlockDrops::Load(csv.path().c_str());
}

} // namespace

// ===========================================================================
// BlockDrops::Get — the lookup the issue asked for
// ===========================================================================

static void test_KnownBlockReturnsItsMappedDrop() {
  TempCsv csv("known",
              "0:0:1,0:0:2\n"
              "10:6,0:11110:2,3\n"
              "110:3,1110:5,6,7\n");
  std::unique_ptr<BlockDrops> drops(BlockDrops::Load(csv.path().c_str()));

  const DropInfo* a = drops->Get(ItemId::pack("0:0:1"));
  CHECK(a != nullptr, "a rule exists for the first row");
  if (a) {
    CHECK_EQ(a->result_id, ItemId::pack("0:0:2"));
    CHECK_EQ(a->count, 1u);
    CHECK_EQ(a->meta, 0u);
  }

  const DropInfo* b = drops->Get(ItemId::pack("10:6"));
  CHECK(b != nullptr, "a rule exists for the second row");
  if (b) {
    CHECK_EQ(b->result_id, ItemId::pack("0:11110:2"));
    CHECK_EQ(b->count, 3u, "a three-column row takes its count from column 3");
    CHECK_EQ(b->meta, 0u);
  }

  const DropInfo* c = drops->Get(ItemId::pack("110:3"));
  CHECK(c != nullptr, "a rule exists for the four-column row");
  if (c) {
    CHECK_EQ(c->result_id, ItemId::pack("1110:5"));
    CHECK_EQ(c->count, 6u);
    CHECK_EQ(c->meta, 7u);
  }
}

static void test_UnknownBlockYieldsTheDocumentedEmptyResult() {
  // BlockDrops.h:16 — "Callers fall back to the block itself when Get()
  // returns nullptr." The documented empty result IS the nullptr, and this
  // is the contract BreakBlockHandler.cpp:106-111 relies on.
  TempCsv csv("unknown", "10:6,0:11110:2\n");
  std::unique_ptr<BlockDrops> drops(BlockDrops::Load(csv.path().c_str()));

  CHECK(drops->Get(ItemId::pack("0:0:1")) == nullptr,
        "an absent source id is not in the table");
  CHECK(drops->Get(0xFFFF) == nullptr, "an id no row mentions is not in the table");
  CHECK(drops->Get(ItemId::pack("10:7")) == nullptr,
        "a RESULT id is not itself a valid lookup key");
}

static void test_EmptyTableMatchesNothing() {
  TempCsv csv("empty", "");
  std::unique_ptr<BlockDrops> drops(BlockDrops::Load(csv.path().c_str()));
  CHECK(drops->Get(0) == nullptr, "an empty table matches nothing, not even id 0");
  CHECK(drops->Get(ItemId::pack("0:0:1")) == nullptr, "an empty table has no packed ids");
}

static void test_MissingFileYieldsEmptyNonNullTable() {
  // Load() allocates the object BEFORE opening the file, so a bad path logs
  // an error and still hands back a usable (empty) table rather than null.
  std::unique_ptr<BlockDrops> drops(
      BlockDrops::Load("/nonexistent/gtnh-definitely-missing-drops.csv"));
  CHECK(drops != nullptr, "a missing file still yields a non-null table");
  if (drops) {
    CHECK(drops->Get(0) == nullptr, "the table is empty");
    CHECK(drops->Get(ItemId::pack("0:0:1")) == nullptr, "the table is empty");
  }
}

static void test_GetPointersAreStableBecauseNothingMutatesAfterLoad() {
  // Get() hands out &it->second into an unordered_map, which unordered_map
  // would invalidate on any rehash. The map is private and the only writer is
  // Load, so the pointers stay valid for the table's lifetime. Pinned so a
  // future "reload in place" API cannot silently break the drop handler,
  // which dereferences the pointer after the call returns.
  TempCsv csv("stable", "0:0:1,0:0:2\n10:6,0:11110:2\n");
  std::unique_ptr<BlockDrops> drops(BlockDrops::Load(csv.path().c_str()));

  const DropInfo* first = drops->Get(ItemId::pack("0:0:1"));
  const DropInfo* again = drops->Get(ItemId::pack("0:0:1"));
  const DropInfo* other = drops->Get(ItemId::pack("10:6"));
  CHECK(first != nullptr);
  CHECK_EQ(first, again, "repeated lookups of one id return the same pointer");
  CHECK_NE(first, other, "different ids return different pointers");
  // Many further lookups must not disturb the earlier pointer.
  for (uint16_t i = 0; i < 512; ++i) (void)drops->Get(i);
  CHECK_EQ(first, drops->Get(ItemId::pack("0:0:1")));
}

// ===========================================================================
// BlockDrops::Load — column arity
// ===========================================================================

static void test_TwoColumnRowDefaultsToCountOneAndMetaZero() {
  std::unique_ptr<BlockDrops> drops(loadCsv("two-col", "5,6\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr);
  if (d) {
    CHECK_EQ(d->result_id, ItemId::pack("6"));
    CHECK_EQ(d->count, 1u, "absent count column defaults to 1");
    CHECK_EQ(d->meta, 0u, "absent meta column defaults to 0");
  }
}

static void test_ThreeColumnRowTakesTheCount() {
  std::unique_ptr<BlockDrops> drops(loadCsv("three-col", "5,6,7\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr);
  if (d) {
    CHECK_EQ(d->result_id, ItemId::pack("6"));
    CHECK_EQ(d->count, 7u);
    CHECK_EQ(d->meta, 0u, "absent meta column defaults to 0");
  }
}

static void test_FourColumnRowTakesCountAndMeta() {
  std::unique_ptr<BlockDrops> drops(loadCsv("four-col", "5,6,7,8\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr);
  if (d) {
    CHECK_EQ(d->result_id, ItemId::pack("6"));
    CHECK_EQ(d->count, 7u);
    CHECK_EQ(d->meta, 8u);
  }
}

static void test_ColumnsBeyondTheFourthAreIgnored() {
  std::unique_ptr<BlockDrops> drops(loadCsv("five-col", "5,6,7,8,9,10\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr);
  if (d) {
    CHECK_EQ(d->count, 7u);
    CHECK_EQ(d->meta, 8u, "the 5th+ columns are silently dropped");
  }
}

// ===========================================================================
// BlockDrops::Load — line filtering
// ===========================================================================

static void test_CommentsAndBlankLinesAreSkipped() {
  std::unique_ptr<BlockDrops> drops(
      loadCsv("comments",
              "# source,result[,count[,meta]]\n"
              "\n"
              "# 0:0:1,0:0:2\n"
              "5,6\n"
              "\n"));
  CHECK(drops->Get(ItemId::pack("5")) != nullptr, "the real row loaded");
  // A comment names ids that must NOT become rules.
  CHECK(drops->Get(ItemId::pack("0:0:1")) == nullptr, "a comment line is not a rule");
  CHECK(drops->Get(0) == nullptr, "a blank line is not a rule keyed to id 0");
}

static void test_IndentedCommentIsNotTreatedAsAComment() {
  // Only line[0] == '#' marks a comment, so a single leading space defeats the
  // check and the line is parsed as a real rule. Here "  # 5,6" splits into
  // ["  # 5", "6"]: ItemId::pack scans for digits and returns 5 for "  # 5",
  // so this line really does create a drop rule — and because emplace
  // keeps the FIRST rule per key, it SHADOWS the genuine "5,7" row below it.
  // QUIRK — pinned as observed, not endorsed.
  std::unique_ptr<BlockDrops> drops(loadCsv("indent-comment", "  # 5,6\n5,7\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr, "an indented comment line still produces a rule");
  if (d) {
    CHECK_EQ(d->result_id, ItemId::pack("6"),
             "the indented-comment rule won because it came first");
    CHECK_EQ(d->count, 1u);
  }
  CHECK(drops->Get(0) == nullptr, "no rule was keyed on block id 0");
}

static void test_TrailingCommentIsNotStrippedButToleratedByStoi() {
  // "5,6 # cobble" — cols[1] is "6 # cobble". ItemId::pack has no colon in it
  // so it scans for digits and stops at the space, and the result is still 6.
  std::unique_ptr<BlockDrops> drops(loadCsv("trailing-comment", "5,6 # cobble\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr, "a mid-line comment does not drop the rule");
  if (d) {
    CHECK_EQ(d->result_id, ItemId::pack("6"));
    CHECK_EQ(d->count, 1u);
  }
}

static void test_TrailingCommentInTheCountColumnDropsTheRule() {
  // stoi("# two") has no leading digits, so it throws and the whole rule is
  // discarded — a commented-out count is not a count.
  std::unique_ptr<BlockDrops> drops(loadCsv("comment-count", "5,6,# two\n"));
  CHECK(drops->Get(ItemId::pack("5")) == nullptr,
        "a '#' in the count column drops the entire rule");
}

static void test_RowsWithFewerThanTwoColumnsAreSkipped() {
  std::unique_ptr<BlockDrops> drops(loadCsv("short-row", "5\n"));
  CHECK(drops->Get(ItemId::pack("5")) == nullptr, "a one-column row is skipped");
  CHECK(drops->Get(5) == nullptr, "and it does not leak in under the plain decimal key");
}

static void test_AllEmptyRowIsSkippedBecauseGetlineDropsTheTrailingField() {
  // A subtle std::getline(ss, cell, ',') behaviour: a trailing delimiter does
  // NOT produce a final empty field, so "," yields ONE column (""), not two.
  // The cols.size() < 2 gate therefore skips the row.
  std::unique_ptr<BlockDrops> only_comma(loadCsv("empty-cells-comma", ",\n"));
  CHECK(only_comma->Get(0) == nullptr,
        "\",\" yields one column and is skipped, so no rule is keyed on id 0");
  CHECK(only_comma->Get(ItemId::pack("0")) == nullptr,
        "and nothing is keyed on the plain decimal zero either");
}

static void test_EmptySourceCellYieldsARuleKeyedOnBlockZero() {
  // ",6" DOES yield two columns (["", "6"]), so it passes the arity gate.
  // ItemId::pack("") returns 0, so the rule is stored keyed on BLOCK ID 0
  // yielding item 6. QUIRK — a blank source column silently means air.
  std::unique_ptr<BlockDrops> drops(loadCsv("empty-cells-src", ",6\n"));
  const DropInfo* d = drops->Get(0);
  CHECK(d != nullptr, "an empty source cell becomes a rule keyed on block id 0");
  if (d) {
    CHECK_EQ(d->result_id, ItemId::pack("6"));
    CHECK_EQ(d->count, 1u);
    CHECK_EQ(d->meta, 0u);
  }
  CHECK(drops->Get(ItemId::pack("6")) == nullptr,
        "the rule is keyed on 0, not on the result id 6");
}

static void test_RowWithATrailingDelimiterIsSkipped() {
  // "5," is the same one-column case as above, so a row with a missing result
  // column is silently dropped rather than creating an air drop.
  std::unique_ptr<BlockDrops> drops(loadCsv("trailing-delim", "5,\n"));
  CHECK(drops->Get(ItemId::pack("5")) == nullptr,
        "a row ending in ',' has no result column and is skipped");
  CHECK(drops->Get(0) == nullptr, "and it creates no id-0 rule either");
}

static void test_NonNumericSourceOrResultPacksToZeroInsteadOfBeingRejected() {
  // QUIRK: only the count/meta columns go through std::stoi, so only THEY
  // can fail validation. ItemId::pack scans for digits and returns 0 when it
  // finds none, so a typo'd id becomes block 0 / item 0 (air) SILENTLY.
  std::unique_ptr<BlockDrops> drops(loadCsv("garbage-ids", "stone,cobblestone\n"));
  const DropInfo* d = drops->Get(0);
  CHECK(d != nullptr, "a non-numeric source id is stored, not rejected");
  if (d) {
    CHECK_EQ(d->result_id, 0u, "and it packs to item id 0");
  }
}

static void test_NonNumericSourceCollidesWithTheRealZeroRow() {
  // Follows from the quirk above: emplace keeps the FIRST row for a key, so
  // a garbage row that packs to 0 can shadow (or be shadowed by) a real
  // block-0 rule depending on file order.
  std::unique_ptr<BlockDrops> first(loadCsv("collide-first", "stone,cobblestone\n0,9\n"));
  if (const DropInfo* d = first->Get(0)) {
    CHECK_EQ(d->result_id, 0u, "the garbage row won because it came first");
  }
  std::unique_ptr<BlockDrops> second(loadCsv("collide-second", "0,9\nstone,cobblestone\n"));
  if (const DropInfo* d = second->Get(0)) {
    CHECK_EQ(d->result_id, ItemId::pack("9"), "the real row won because it came first");
  }
}

// ===========================================================================
// BlockDrops::Load — numeric coercion and failures
// ===========================================================================

static void test_NonNumericCountDropsTheWholeRule() {
  std::unique_ptr<BlockDrops> drops(loadCsv("nan-count", "5,6,two\n"));
  CHECK(drops->Get(ItemId::pack("5")) == nullptr,
        "an unparseable count drops the entire rule");
}

static void test_NonNumericMetaDropsTheWholeRule() {
  std::unique_ptr<BlockDrops> drops(loadCsv("nan-meta", "5,6,7,eight\n"));
  CHECK(drops->Get(ItemId::pack("5")) == nullptr,
        "an unparseable meta drops the entire rule");
}

static void test_BadRowDoesNotAffectNeighbouringRows() {
  std::unique_ptr<BlockDrops> drops(
      loadCsv("bad-row-neighbours",
              "1,2,3\n"
              "4,5,notanumber\n"
              "6,7,8\n"));
  const DropInfo* a = drops->Get(ItemId::pack("1"));
  const DropInfo* b = drops->Get(ItemId::pack("6"));
  CHECK(a != nullptr, "the row before the bad one loaded");
  CHECK(b != nullptr, "the row after the bad one loaded");
  if (a) CHECK_EQ(a->count, 3u);
  if (b) CHECK_EQ(b->count, 8u);
  CHECK(drops->Get(ItemId::pack("4")) == nullptr, "only the bad row is missing");
}

static void test_CountOutOfUint8RangeWraps() {
  // static_cast<uint8_t>(300) == 44 — silent truncation, no warning logged.
  std::unique_ptr<BlockDrops> drops(loadCsv("count-wrap", "5,6,300\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr, "a 300-item count is accepted");
  if (d) CHECK_EQ(d->count, 44u, "300 truncates to 44");
}

static void test_MetaOutOfUint8RangeWraps() {
  std::unique_ptr<BlockDrops> drops(loadCsv("meta-wrap", "5,6,7,300\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr, "a meta of 300 is accepted");
  if (d) CHECK_EQ(d->meta, 44u, "300 truncates to 44");
}

static void test_NegativeCountWrapsToTwoFiftyFive() {
  std::unique_ptr<BlockDrops> drops(loadCsv("count-negative", "5,6,-1\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr, "a negative count is accepted");
  if (d) CHECK_EQ(d->count, 255u, "-1 becomes 255");
}

static void test_CountOutOfIntRangeDropsTheRule() {
  // std::stoi throws std::out_of_range past INT_MAX — caught, rule dropped.
  // Distinct from the uint8 wrap above: 300 survives, 99999999999 does not.
  std::unique_ptr<BlockDrops> drops(loadCsv("count-int-overflow", "5,6,99999999999\n"));
  CHECK(drops->Get(ItemId::pack("5")) == nullptr,
        "a count past INT_MAX is rejected by stoi, not truncated");
}

static void test_CountWithLeadingWhitespaceStillParses() {
  std::unique_ptr<BlockDrops> drops(loadCsv("count-spaces", "5,6, 12\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr, "stoi skips leading whitespace");
  if (d) CHECK_EQ(d->count, 12u);
}

static void test_CountWithTrailingGarbageTakesTheLeadingDigits() {
  std::unique_ptr<BlockDrops> drops(loadCsv("count-trailing", "5,6,12abc\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr, "stoi stops at the first non-digit");
  if (d) CHECK_EQ(d->count, 12u, "12abc parses as 12");
}

static void test_CrlfRowsStillLoad() {
  std::unique_ptr<BlockDrops> drops(loadCsv("crlf", "5,6,7,8\r\n9,10,11,12\r\n"));
  const DropInfo* a = drops->Get(ItemId::pack("5"));
  CHECK(a != nullptr, "a CRLF row loads");
  if (a) {
    CHECK_EQ(a->result_id, ItemId::pack("6"), "the CR is not folded into the id");
    CHECK_EQ(a->count, 7u);
    CHECK_EQ(a->meta, 8u);
  }
  const DropInfo* b = drops->Get(ItemId::pack("9"));
  CHECK(b != nullptr, "the second CRLF row loads");
  if (b) {
    CHECK_EQ(b->result_id, ItemId::pack("10"));
    CHECK_EQ(b->count, 11u);
    CHECK_EQ(b->meta, 12u);
  }
}

static void test_LastLineWithoutATrailingNewlineStillLoads() {
  // The shipped drops.csv has no trailing newline on its last row.
  std::unique_ptr<BlockDrops> drops(loadCsv("no-trailing-nl", "5,6\n9,10"));
  CHECK(drops->Get(ItemId::pack("9")) != nullptr, "an unterminated final row loads");
  if (const DropInfo* d = drops->Get(ItemId::pack("9"))) {
    CHECK_EQ(d->result_id, ItemId::pack("10"));
  }
}

static void test_DuplicateSourceKeepsTheFirstRule() {
  std::unique_ptr<BlockDrops> drops(loadCsv("dupe", "5,6,1\n5,7,2\n"));
  const DropInfo* d = drops->Get(ItemId::pack("5"));
  CHECK(d != nullptr);
  if (d) {
    CHECK_EQ(d->result_id, ItemId::pack("6"), "the first rule for a source wins");
    CHECK_EQ(d->count, 1u);
  }
}

static void test_MultipleRulesCoexist() {
  std::unique_ptr<BlockDrops> drops(loadCsv("multi", "1,2\n3,4,5\n6,7,8,9\n"));
  CHECK(drops->Get(ItemId::pack("1")) != nullptr);
  CHECK(drops->Get(ItemId::pack("3")) != nullptr);
  CHECK(drops->Get(ItemId::pack("6")) != nullptr);
  CHECK(drops->Get(ItemId::pack("2")) == nullptr,
        "a result id is not implicitly a source id");
}

static void test_RuleIsDirectionalAndNotSymmetric() {
  // "a -> b" must not imply "b -> a".
  std::unique_ptr<BlockDrops> drops(loadCsv("directional", "1,2\n"));
  CHECK(drops->Get(ItemId::pack("1")) != nullptr);
  CHECK(drops->Get(ItemId::pack("2")) == nullptr, "the mapping is one-way");
}

// ===========================================================================
// BlockDrops::Load — id decoding
// ===========================================================================

static void test_SourceAndResultIdsAreDecodedByItemIdPack() {
  // The loader hands both id columns straight to ItemId::pack, so the CSV
  // speaks packed ids, not plain numbers.
  std::unique_ptr<BlockDrops> drops(
      loadCsv("packed", "0:0:1,0:0:2\n10:6,0:11110:2,4\n110:3,1110:5,6,7\n"));
  // Hand-computed from ItemId::pack: prefix bits shift into the top `plen`
  // bits, the decimal payload fills the rest.
  CHECK_EQ(ItemId::pack("0:0:1"), 1u, "pack sanity: prefix 0:0, payload 1");
  CHECK_EQ(ItemId::pack("0:0:2"), 2u, "pack sanity: prefix 0:0, payload 2");
  CHECK_EQ(ItemId::pack("10:6"), 32774u, "pack sanity: prefix 10 (2), payload 6");
  CHECK_EQ(ItemId::pack("0:11110:2"), 30722u, "pack sanity: prefix 0:11110 (30), payload 2");

  const DropInfo* a = drops->Get(ItemId::pack("0:0:1"));
  if (a) CHECK_EQ(a->result_id, ItemId::pack("0:0:2"));
  const DropInfo* b = drops->Get(ItemId::pack("10:6"));
  if (b) {
    CHECK_EQ(b->result_id, ItemId::pack("0:11110:2"));
    CHECK_EQ(b->count, 4u, "packed ids do not consume the count column");
  }
  const DropInfo* c = drops->Get(ItemId::pack("110:3"));
  if (c) {
    CHECK_EQ(c->result_id, ItemId::pack("1110:5"));
    CHECK_EQ(c->count, 6u);
    CHECK_EQ(c->meta, 7u);
  }
}

static void test_PackedIdWithTooManyPrefixBitsCollapsesToZero() {
  // ItemId::pack returns 0 past 15 prefix bits, so a malformed id becomes
  // block 0 rather than failing.
  std::unique_ptr<BlockDrops> drops(loadCsv("long-prefix", "11111111111111111:1,2\n"));
  CHECK(drops->Get(0) != nullptr,
        "an over-long prefix is stored under id 0 instead of being rejected");
}

// ===========================================================================
// The shipped table
// ===========================================================================

static void test_ShippedRegistryDropsCsvMatchesTheExpectedTable() {
  // src/content/data/registry/drops.csv as shipped (185 bytes, no trailing
  // newline on the last row):
  //   # source,result[,count[,meta]]
  //   # Breaking a block yields its mapped drop instead of the block itself.
  //   # Ids are packed item ids (see data/registry/items.csv).
  //   0:0:1,0:0:2
  //   10:6,0:11110:2
  // Both rows are TWO-COLUMN, so both drops count 1 — the "2" in the second
  // row is part of the result id 0:11110:2, not a count column.
  std::unique_ptr<BlockDrops> drops(
      BlockDrops::Load(REGISTRY_DROPS_DIR "/drops.csv"));
  CHECK(drops != nullptr);
  if (!drops) return;

  const DropInfo* a = drops->Get(ItemId::pack("0:0:1"));
  CHECK(a != nullptr, "the shipped table maps 0:0:1");
  if (a) {
    CHECK_EQ(a->result_id, ItemId::pack("0:0:2"));
    CHECK_EQ(a->count, 1u, "a two-column row drops one item");
    CHECK_EQ(a->meta, 0u);
  }

  const DropInfo* b = drops->Get(ItemId::pack("10:6"));
  CHECK(b != nullptr, "the shipped table maps 10:6");
  if (b) {
    CHECK_EQ(b->result_id, ItemId::pack("0:11110:2"));
    CHECK_EQ(b->count, 1u, "the trailing :2 belongs to the result id, not a count");
    CHECK_EQ(b->meta, 0u);
  }

  // A drop rule on air would need a source column of "0". The shipped table
  // has none, and BreakBlockHandler guards the air case with
  // `broken_block != 0` anyway, so this is unreachable today; pinned so a
  // future air-drop feature has to notice the table accepts such a row.
  CHECK(drops->Get(0) == nullptr, "the shipped table has no rule keyed on block 0");
  CHECK(drops->Get(ItemId::pack("0:0:2")) == nullptr,
        "the shipped table has no rule for the RESULT of its first row");
  CHECK(drops->Get(ItemId::pack("0:11110:2")) == nullptr,
        "the shipped table has no rule for the RESULT of its second row");
}

// ===========================================================================
// The process-wide singleton
// ===========================================================================

static void test_SetInstanceAndInstanceRoundTrip() {
  DropsGuard guard;
  CHECK(BlockDrops::instance() == nullptr, "the guard starts from a clean slate");

  TempCsv csv("singleton", "5,6,7,8\n");
  std::unique_ptr<BlockDrops> drops(BlockDrops::Load(csv.path().c_str()));
  BlockDrops::setInstance(drops.get());

  CHECK_EQ(BlockDrops::instance(), drops.get(), "setInstance publishes the pointer");
  if (BlockDrops::instance()) {
    const DropInfo* d = BlockDrops::instance()->Get(ItemId::pack("5"));
    CHECK(d != nullptr, "the published table is usable through the singleton");
    if (d) CHECK_EQ(d->count, 7u);
  }

  BlockDrops::setInstance(nullptr);
  CHECK(BlockDrops::instance() == nullptr, "the singleton can be cleared again");
}

static void test_SetInstanceDoesNotTakeOwnership() {
  // setInstance stores a raw pointer and never deletes it, so the object has
  // to outlive the singleton; simcore/main.cpp:155-156 keeps its Load() result
  // for the whole process lifetime, which is what makes that safe there.
  //
  // Checked with a STACK-ALLOCATED table: if setInstance took ownership (or
  // deleted its argument), this scope exit would double-free. Reaching the end
  // of the scope with a readable table is the evidence. (An earlier version of
  // this test released the owning unique_ptr and then read through the
  // singleton — a deliberate use-after-free that segfaulted; ownership
  // transfer is not something that can be observed from the outside, so the
  // check is written this way instead.)
  DropsGuard guard;
  BlockDrops* stack_table_address = nullptr;
  {
    BlockDrops stack_table;
    stack_table_address = &stack_table;
    BlockDrops::setInstance(&stack_table);
    CHECK_EQ(BlockDrops::instance(), &stack_table,
             "setInstance publishes exactly the pointer it was given");
    CHECK(BlockDrops::instance() != nullptr);
    if (BlockDrops::instance()) {
      CHECK(BlockDrops::instance()->Get(0) == nullptr,
            "a default-constructed table is empty and readable through the singleton");
      CHECK(BlockDrops::instance()->Get(ItemId::pack("0:0:1")) == nullptr,
            "and matches nothing");
    }
    // No delete happens here; if setInstance owned the table this would be a
    // double free at the closing brace.
  }
  // The stack object is gone, but setInstance never clears the pointer, so the
  // singleton is left DANGLING. Only its address is compared here (no
  // dereference), which is well defined. This is the real hazard of the
  // raw-pointer singleton: whoever calls setInstance is responsible for
  // clearing it, which is exactly what DropsGuard does on scope exit.
  CHECK_EQ(BlockDrops::instance(), stack_table_address,
           "setInstance left a stale pointer behind after the table died");
  BlockDrops::setInstance(nullptr);
  CHECK(BlockDrops::instance() == nullptr, "the pointer can be cleared again");
}

// ===========================================================================
// QUIRKS (observed, reported to beads, NOT worked around here)
//
//  1. Only the count/meta columns are validated. A typo in the source or
//     result column never reaches the "skipping bad line" warning at all —
//     ItemId::pack scans for digits and returns 0 when it finds none, so
//     "stone" becomes block id 0 and "cobblestone" becomes item id 0 (air).
//     The rule is stored anyway, and emplace-first-wins means such a row can
//     shadow a real block-0 rule.  BlockDrops.cpp:31-39
//  2. uint8 count/meta truncate instead of being rejected (300 -> 44), so a
//     typo'd count silently changes the drop quantity. Only a value past
//     INT_MAX is rejected, and then by stoi rather than by design.
//     BlockDrops.cpp:34-35
//  3. The parsed meta column is never USED: the drop path in
//     BreakBlockHandler.cpp:106-111 copies d->result_id and d->count but not
//     d->meta, so the fourth drops.csv column is parsed, stored and dropped.
//  4. Get() hands out a pointer into an unordered_map. That is only safe
//     because the map is private and written only by Load(); callers
//     dereference the pointer after return, so a future in-place reload
//     would be a use-after-free.  BlockDrops.cpp:46-49
// ===========================================================================

int main() {
  TEST(KnownBlockReturnsItsMappedDrop);
  TEST(UnknownBlockYieldsTheDocumentedEmptyResult);
  TEST(EmptyTableMatchesNothing);
  TEST(MissingFileYieldsEmptyNonNullTable);
  TEST(GetPointersAreStableBecauseNothingMutatesAfterLoad);

  TEST(TwoColumnRowDefaultsToCountOneAndMetaZero);
  TEST(ThreeColumnRowTakesTheCount);
  TEST(FourColumnRowTakesCountAndMeta);
  TEST(ColumnsBeyondTheFourthAreIgnored);

  TEST(CommentsAndBlankLinesAreSkipped);
  TEST(IndentedCommentIsNotTreatedAsAComment);
  TEST(TrailingCommentIsNotStrippedButToleratedByStoi);
  TEST(TrailingCommentInTheCountColumnDropsTheRule);
  TEST(RowsWithFewerThanTwoColumnsAreSkipped);
  TEST(AllEmptyRowIsSkippedBecauseGetlineDropsTheTrailingField);
  TEST(EmptySourceCellYieldsARuleKeyedOnBlockZero);
  TEST(RowWithATrailingDelimiterIsSkipped);
  TEST(NonNumericSourceOrResultPacksToZeroInsteadOfBeingRejected);
  TEST(NonNumericSourceCollidesWithTheRealZeroRow);

  TEST(NonNumericCountDropsTheWholeRule);
  TEST(NonNumericMetaDropsTheWholeRule);
  TEST(BadRowDoesNotAffectNeighbouringRows);
  TEST(CountOutOfUint8RangeWraps);
  TEST(MetaOutOfUint8RangeWraps);
  TEST(NegativeCountWrapsToTwoFiftyFive);
  TEST(CountOutOfIntRangeDropsTheRule);
  TEST(CountWithLeadingWhitespaceStillParses);
  TEST(CountWithTrailingGarbageTakesTheLeadingDigits);
  TEST(CrlfRowsStillLoad);
  TEST(LastLineWithoutATrailingNewlineStillLoads);
  TEST(DuplicateSourceKeepsTheFirstRule);
  TEST(MultipleRulesCoexist);
  TEST(RuleIsDirectionalAndNotSymmetric);

  TEST(SourceAndResultIdsAreDecodedByItemIdPack);
  TEST(PackedIdWithTooManyPrefixBitsCollapsesToZero);

  TEST(ShippedRegistryDropsCsvMatchesTheExpectedTable);

  TEST(SetInstanceAndInstanceRoundTrip);
  TEST(SetInstanceDoesNotTakeOwnership);

  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
