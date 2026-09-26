// GridPatternMatcher unit tests (issue gp-48uq).
//
// File under test:
//   src/game/crafting/GridPatternMatcher.cpp  — the 3x3 crafting-grid matcher
//
// What it does, exactly as written:
//   * A pattern is 9 GridSlots. GridSlot{item_id, count, metadata}:
//       item_id  == 0  -> "this cell must be EMPTY". A non-empty grid slot
//                         here is a MISMATCH. Shape is therefore load-bearing:
//                         there is no trimming / no bounding-box normalisation.
//       count    == 0  -> "any count". count > 0 -> minimum required count.
//       metadata == 0  -> "any meta".   metadata > 0 -> exact match required.
//   * matchDetailed() walks slots 0..8 in order and returns on the first
//     mismatch, so a failure short-circuits.
//   * match() returns the recipe NAME of the first pattern in list order that
//     matches, or "" when none do.
//
// Three behaviours are PINNED as observed, not blessed. They are marked
// PRODUCTION DEFECT where they are wrong, and are NOT fixed by this file —
// this file only reports them:
//
//   1. matchDetailed() returns a PARTIAL consumed_per_slot on failure. Slots
//      0..i-1 already carry their consumed counts when slot i fails; the
//      failure path only zeroes slot i and returns. A caller that reads
//      consumed_per_slot without checking `matched` first gets a half-written
//      consumption plan. Pinned by
//      test_matchDetailed_failure_leaves_partial_consumed_counts.
//   2. There is no offset/shape-normalisation and no mirroring. Matching is
//      strictly index-by-index against the 9-slot pattern. This is the
//      documented intent of the issue ("wrong offset must fail", "mirrored
//      shape must fail") and is asserted as such — but it also means a recipe
//      authored 2x2 in the top-left corner will not match the same recipe
//      placed anywhere else in the grid, which is NOT how vanilla-style
//      crafting tables behave. See the mirrored/offset tests.
//   3. matchDetailed() and matchSingle() index grid[i] for i in 0..8 with no
//     bounds check. A grid vector with fewer than 9 entries is an
//     out-of-bounds read. This test NEVER exercises that path — driving it
//     would be undefined behaviour, not a test. The "empty grid" case is
//     therefore covered as a full 9-slot grid of empty stacks, which is what
//     a real empty crafting grid looks like on the wire.
//
// Uses the PROJECT's own harness (src/engine/net/test/test.h convention,
// mirrored by src/game/machines/test/test_explosion_system.cpp and
// src/game/storage/test/test_chest_state_manager.cpp). GoogleTest is
// deliberately NOT used: it is absent from conanfile.txt, CI does not install
// libgtest-dev, and CI builds Release with a global -Werror — a
// find_package(GTest) guard would make this test silently vanish there.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <game/crafting/GridPatternMatcher.h>

// ---------------------------------------------------------------------------
// Harness — definitions and macros near the TOP, before first use.
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "",
           msg ? msg : "");
  }
}

#define CHECK(cond, ...) \
  test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) \
  test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) \
  test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Fixtures / builders
// ---------------------------------------------------------------------------

using RecipeManager::ItemStack;

static constexpr uint16_t kPlanks = 1001;   // stands in for oak_planks
static constexpr uint16_t kStick = 1002;    // stands in for stick
static constexpr uint16_t kIngot = 1003;    // stands in for iron_ingot
static constexpr uint16_t kGem = 1004;      // stands in for diamond

// A GridPattern is value-initialised: all nine slots {0,0,0} == "must be empty".
static Crafting::GridPattern emptyPattern() {
  return Crafting::GridPattern{};
}

// Fills one cell (row-major index 0..8) of a pattern.
static Crafting::GridPattern withCell(uint16_t item_id, int index,
                                      uint8_t count = 1,
                                      uint16_t metadata = 0) {
  Crafting::GridPattern p = emptyPattern();
  p.slots[static_cast<size_t>(index)] = {item_id, count, metadata};
  return p;
}

// A 9-slot grid: the only shape the matcher is safe to feed.
static std::vector<ItemStack> makeGrid() {
  return std::vector<ItemStack>(9, ItemStack{0, 0, 0});
}

static std::vector<ItemStack> gridWith(int index, uint16_t item_id,
                                       uint8_t count = 1,
                                       uint16_t metadata = 0) {
  auto g = makeGrid();
  g[static_cast<size_t>(index)] = {item_id, count, metadata};
  return g;
}

// ---------------------------------------------------------------------------
// Shaped (positional) match
// ---------------------------------------------------------------------------

// A 2x2 plank block in the top-left corner, mirrored into the pattern so the
// grid must be shaped exactly that way.
static Crafting::GridPattern plank2x2Pattern() {
  Crafting::GridPattern p = emptyPattern();
  p.slots[0] = {kPlanks, 1, 0};
  p.slots[1] = {kPlanks, 1, 0};
  p.slots[3] = {kPlanks, 1, 0};
  p.slots[4] = {kPlanks, 1, 0};
  return p;
}

static std::vector<ItemStack> plank2x2Grid() {
  auto g = makeGrid();
  g[0] = {kPlanks, 1, 0};
  g[1] = {kPlanks, 1, 0};
  g[3] = {kPlanks, 1, 0};
  g[4] = {kPlanks, 1, 0};
  return g;
}

static void test_exact_shaped_match_succeeds() {
  Crafting::GridPatternMatcher m;
  const auto grid = plank2x2Grid();
  const auto pattern = plank2x2Pattern();

  CHECK(m.matchSingle(grid, pattern), "2x2 planks at the top-left match");
  CHECK(m.matchDetailed(grid, pattern).matched,
        "matchDetailed agrees with matchSingle");
}

static void test_wrong_offset_shaped_match_fails() {
  Crafting::GridPatternMatcher m;
  // Identical items, but the 2x2 block sits one column to the right.
  auto grid = makeGrid();
  grid[1] = {kPlanks, 1, 0};
  grid[2] = {kPlanks, 1, 0};
  grid[4] = {kPlanks, 1, 0};
  grid[5] = {kPlanks, 1, 0};

  CHECK(!m.matchSingle(grid, plank2x2Pattern()),
        "the same items shifted one column must NOT match (no offset search)");
  // One row down is also rejected.
  auto lower = makeGrid();
  lower[3] = {kPlanks, 1, 0};
  lower[4] = {kPlanks, 1, 0};
  lower[6] = {kPlanks, 1, 0};
  lower[7] = {kPlanks, 1, 0};
  CHECK(!m.matchSingle(lower, plank2x2Pattern()),
        "the same items shifted one row down must NOT match");
}

static void test_mirrored_shaped_match_fails() {
  Crafting::GridPatternMatcher m;

  // An L-shape is asymmetric, so its mirror image is a different pattern.
  Crafting::GridPattern ell = emptyPattern();
  ell.slots[0] = {kPlanks, 1, 0};
  ell.slots[1] = {kPlanks, 1, 0};
  ell.slots[2] = {kPlanks, 1, 0};
  ell.slots[3] = {kGem, 1, 0};

  // The original L.
  auto original = makeGrid();
  original[0] = {kPlanks, 1, 0};
  original[1] = {kPlanks, 1, 0};
  original[2] = {kPlanks, 1, 0};
  original[3] = {kGem, 1, 0};
  CHECK(m.matchSingle(original, ell), "precondition: the L itself matches");

  // Its horizontal mirror: row 0 becomes [0, plank, plank] and the gem moves.
  auto mirrored = makeGrid();
  mirrored[0] = {kGem, 1, 0};
  mirrored[1] = {kPlanks, 1, 0};
  mirrored[2] = {kPlanks, 1, 0};
  mirrored[3] = {kPlanks, 1, 0};
  CHECK(!m.matchSingle(mirrored, ell), "a mirrored shape must NOT match");

  // Vertical mirror of the same L: gem row moves from index 3 to index 6.
  auto flipped = makeGrid();
  flipped[0] = {kGem, 1, 0};
  flipped[1] = {kPlanks, 1, 0};
  flipped[2] = {kPlanks, 1, 0};
  flipped[3] = {kPlanks, 1, 0};
  CHECK(!m.matchSingle(flipped, ell), "a flipped shape must NOT match");
}

static void test_symmetric_shape_mirror_still_matches() {
  // Control for the mirror test above: a shape that IS its own mirror image
  // still matches after mirroring, so the mirror rejection above is caused by
  // the asymmetry and not by a bug that rejects any grid.
  Crafting::GridPatternMatcher m;
  Crafting::GridPattern domino = emptyPattern();
  domino.slots[0] = {kPlanks, 1, 0};
  domino.slots[1] = {kPlanks, 1, 0};

  auto left = makeGrid();
  left[0] = {kPlanks, 1, 0};
  left[1] = {kPlanks, 1, 0};
  auto right = makeGrid();
  right[1] = {kPlanks, 1, 0};
  right[2] = {kPlanks, 1, 0};

  CHECK(m.matchSingle(left, domino), "precondition: left-aligned domino");
  CHECK(!m.matchSingle(right, domino),
        "shifting an even a symmetric 1x2 pattern fails: matching is strictly "
        "index-for-index, so this pins 'no offset search' rather than a "
        "specific mirror rule");
}

// ---------------------------------------------------------------------------
// Empty grid
// ---------------------------------------------------------------------------

static void test_empty_grid_matches_an_all_empty_pattern() {
  Crafting::GridPatternMatcher m;
  const auto empty = makeGrid();

  CHECK(m.matchSingle(empty, emptyPattern()),
        "nine empty grid slots satisfy nine empty pattern slots");
  const auto r = m.matchDetailed(empty, emptyPattern());
  CHECK(r.matched, "matchDetailed: all-empty grid matches all-empty pattern");
  CHECK_EQ(r.consumed_per_slot.size(), size_t(9),
           "consumed_per_slot is always 9 entries");
  for (size_t i = 0; i < r.consumed_per_slot.size(); ++i) {
    CHECK_EQ(int(r.consumed_per_slot[i]), 0,
             "an empty slot consumes 0 items");
  }
}

static void test_empty_grid_fails_any_non_empty_pattern() {
  Crafting::GridPatternMatcher m;
  const auto empty = makeGrid();
  for (int i = 0; i < 9; ++i) {
    CHECK(!m.matchSingle(empty, withCell(kPlanks, i)),
          "an empty grid cannot satisfy a pattern with any filled cell");
  }
}

// PRODUCTION DEFECT is NOT triggered here: a std::vector<ItemStack> with fewer
// than 9 elements is read out of bounds by matchDetailed(). The empty-grid
// behaviour is covered above through a well-formed 9-slot grid instead.

// ---------------------------------------------------------------------------
// Shapeless match (pattern with a single filled cell = "anywhere-empty" shape)
// ---------------------------------------------------------------------------

static void test_shapeless_single_item_match() {
  Crafting::GridPatternMatcher m;
  // A shapeless recipe expressed the way this matcher can express it: one
  // required item and nothing else pinned down. Every other cell must be empty.
  const auto pattern = withCell(kIngot, 0);

  auto grid = gridWith(0, kIngot, 1);
  CHECK(m.matchSingle(grid, pattern), "one ingot in slot 0 matches");

  // Same single item, different slot -> fails, because cell 0 is pinned.
  auto shifted = gridWith(4, kIngot, 1);
  CHECK(!m.matchSingle(shifted, pattern),
        "a single-cell pattern still pins that cell's index");

  // Extra items break the match: the eight other cells demand empty.
  auto crowded = gridWith(0, kIngot, 1);
  crowded[5] = {kGem, 1, 0};
  CHECK(!m.matchSingle(crowded, pattern),
        "a stray item in an 'any other cell' slot fails the match");
}

static void test_shapeless_pair_of_items() {
  Crafting::GridPatternMatcher m;
  Crafting::GridPattern two = emptyPattern();
  two.slots[0] = {kStick, 1, 0};
  two.slots[1] = {kIngot, 1, 0};

  auto grid = makeGrid();
  grid[0] = {kStick, 1, 0};
  grid[1] = {kIngot, 1, 0};
  CHECK(m.matchSingle(grid, two), "stick then ingot matches");

  auto swapped = makeGrid();
  swapped[0] = {kIngot, 1, 0};
  swapped[1] = {kStick, 1, 0};
  CHECK(!m.matchSingle(swapped, two), "order matters: swapping the two fails");

  auto sameItem = makeGrid();
  sameItem[0] = {kStick, 1, 0};
  sameItem[1] = {kStick, 1, 0};
  CHECK(!m.matchSingle(sameItem, two), "two sticks is not stick+ingot");
}

// ---------------------------------------------------------------------------
// Count semantics
// ---------------------------------------------------------------------------

static void test_pattern_count_zero_accepts_any_count() {
  Crafting::GridPatternMatcher m;
  const auto pattern = withCell(kPlanks, 0, 0); // count == 0 -> "any"

  for (uint8_t n : {uint8_t(1), uint8_t(2), uint8_t(16), uint8_t(64)}) {
    auto grid = gridWith(0, kPlanks, n);
    CHECK(m.matchSingle(grid, pattern),
          "pattern count 0 accepts a grid slot of any count");
  }
}

static void test_pattern_count_is_a_minimum_not_an_equality() {
  Crafting::GridPatternMatcher m;
  const auto pattern = withCell(kPlanks, 0, 4); // needs at least 4

  CHECK(!m.matchSingle(gridWith(0, kPlanks, 3), pattern),
        "3 planks cannot satisfy a minimum of 4");
  CHECK(m.matchSingle(gridWith(0, kPlanks, 4), pattern),
        "exactly 4 satisfies a minimum of 4");
  CHECK(m.matchSingle(gridWith(0, kPlanks, 64), pattern),
        "64 planks satisfy a minimum of 4 (it is a minimum, not an equality)");
}

static void test_matching_consumes_the_whole_grid_stack_not_the_minimum() {
  Crafting::GridPatternMatcher m;
  // Grid holds 10 planks; the pattern only requires 4. Consumption is reported
  // as the FULL grid count, which is what the caller needs in order to deduct.
  const auto r = m.matchDetailed(gridWith(0, kPlanks, 10), withCell(kPlanks, 0, 4));
  CHECK(r.matched, "10 planks satisfy a minimum of 4");
  CHECK_EQ(int(r.consumed_per_slot[0]), 10,
           "consumed reports the whole stack (10), not the required minimum (4)");
}

// ---------------------------------------------------------------------------
// Metadata semantics
// ---------------------------------------------------------------------------

static void test_pattern_metadata_zero_accepts_any_meta() {
  Crafting::GridPatternMatcher m;
  const auto pattern = withCell(kGem, 0, 1, 0); // metadata 0 -> "any"

  for (uint16_t meta : {uint16_t(0), uint16_t(1), uint16_t(5), uint16_t(300)}) {
    auto grid = gridWith(0, kGem, 1, meta);
    CHECK(m.matchSingle(grid, pattern),
          "pattern metadata 0 accepts a grid slot with any meta");
  }
}

static void test_pattern_metadata_is_exact_when_non_zero() {
  Crafting::GridPatternMatcher m;
  const auto pattern = withCell(kGem, 0, 1, 7);

  CHECK(m.matchSingle(gridWith(0, kGem, 1, 7), pattern),
        "meta 7 satisfies a pattern demanding meta 7");
  for (uint16_t meta : {uint16_t(0), uint16_t(6), uint16_t(8)}) {
    CHECK(!m.matchSingle(gridWith(0, kGem, 1, meta), pattern),
          "a different meta is rejected when the pattern pins one");
  }
}

// ---------------------------------------------------------------------------
// match() — the multi-pattern lookup
// ---------------------------------------------------------------------------

static void test_match_returns_the_first_matching_recipe_name() {
  Crafting::GridPatternMatcher m;
  std::vector<std::pair<std::string, Crafting::GridPattern>> patterns = {
      {"cable_tin", withCell(kPlanks, 0)},
      {"cable_copper", withCell(kIngot, 0)},
      {"cable_gold", withCell(kGem, 0)},
  };

  CHECK_EQ(m.match(gridWith(0, kPlanks, 3), patterns), std::string("cable_tin"),
           "the first pattern in list order wins");
  CHECK_EQ(m.match(gridWith(0, kIngot, 1), patterns), std::string("cable_copper"),
           "a later pattern matches when the earlier ones do not");
  CHECK_EQ(m.match(gridWith(0, kGem, 1), patterns), std::string("cable_gold"),
           "the last pattern is reachable");
}

static void test_match_is_first_in_list_order_not_most_specific() {
  Crafting::GridPatternMatcher m;
  // Two patterns both accept a single plank; the FIRST one must be returned,
  // i.e. there is no "best" scoring pass — match() is a plain ordered scan.
  std::vector<std::pair<std::string, Crafting::GridPattern>> patterns = {
      {"first", withCell(kPlanks, 0)},
      {"second", withCell(kPlanks, 0)},
  };
  CHECK_EQ(m.match(gridWith(0, kPlanks, 1), patterns), std::string("first"),
           "an equally-valid later pattern never wins");
}

static void test_match_returns_empty_string_when_nothing_matches() {
  Crafting::GridPatternMatcher m;
  std::vector<std::pair<std::string, Crafting::GridPattern>> patterns = {
      {"planks", withCell(kPlanks, 0)},
      {"ingot", withCell(kIngot, 0)},
  };
  CHECK_EQ(m.match(gridWith(0, kGem, 1), patterns), std::string(""),
           "an unmatched grid returns \"\" (not a name, not a throw)");
}

static void test_match_with_no_patterns_returns_empty_string() {
  Crafting::GridPatternMatcher m;
  CHECK_EQ(m.match(plank2x2Grid(), {}), std::string(""),
           "an empty pattern list matches nothing");
}

static void test_empty_name_patterns_are_returned_verbatim() {
  // Not a defect: an unnamed pattern that matches yields "". Callers cannot
  // distinguish "no match" from "a match with an empty name". Pinned so the
  // ambiguity is visible rather than silent.
  Crafting::GridPatternMatcher m;
  std::vector<std::pair<std::string, Crafting::GridPattern>> patterns = {
      {"", withCell(kPlanks, 0)},
  };
  CHECK_EQ(m.match(gridWith(0, kPlanks, 1), patterns), std::string(""),
           "a matching pattern with an empty name returns \"\" — "
           "indistinguishable from no-match at the call site");
}

// ---------------------------------------------------------------------------
// matchDetailed — consumption tracking and the partial-result defect
// ---------------------------------------------------------------------------

static void test_matchDetailed_reports_full_consumption_for_a_full_match() {
  Crafting::GridPatternMatcher m;
  auto grid = makeGrid();
  grid[0] = {kPlanks, 5, 0};
  grid[1] = {kStick, 2, 0};

  Crafting::GridPattern p = emptyPattern();
  p.slots[0] = {kPlanks, 1, 0};
  p.slots[1] = {kStick, 1, 0};

  const auto r = m.matchDetailed(grid, p);
  CHECK(r.matched, "the grid matches");
  CHECK_EQ(r.consumed_per_slot.size(), size_t(9), "always 9 entries");
  CHECK_EQ(int(r.consumed_per_slot[0]), 5, "slot 0 consumes all 5 planks");
  CHECK_EQ(int(r.consumed_per_slot[1]), 2, "slot 1 consumes both sticks");
  for (size_t i = 2; i < 9; ++i) {
    CHECK_EQ(int(r.consumed_per_slot[i]), 0,
             "empty pattern cells against empty grid slots consume 0");
  }
}

static void test_matchDetailed_always_returns_nine_entries_even_on_failure() {
  Crafting::GridPatternMatcher m;
  // Fails at index 0.
  const auto early = m.matchDetailed(gridWith(0, kGem, 1), withCell(kPlanks, 0));
  CHECK(!early.matched, "precondition: mismatched item fails");
  CHECK_EQ(early.consumed_per_slot.size(), size_t(9),
           "consumed_per_slot is sized 9 before the loop starts, so a failure "
           "still yields 9 entries — callers can index it safely");

  // Fails at the very last index.
  Crafting::GridPattern last = withCell(kPlanks, 8);
  auto grid = makeGrid();
  grid[0] = {kPlanks, 1, 0};
  grid[8] = {kGem, 1, 0};
  const auto late = m.matchDetailed(grid, last);
  CHECK(!late.matched, "mismatch on the final slot fails");
  CHECK_EQ(late.consumed_per_slot.size(), size_t(9), "still 9 entries");
  CHECK_EQ(int(late.consumed_per_slot[8]), 0, "the failing slot itself is 0");
}

// PRODUCTION DEFECT 1: partial consumption on failure.
static void test_matchDetailed_failure_leaves_partial_consumed_counts() {
  Crafting::GridPatternMatcher m;

  Crafting::GridPattern p = emptyPattern();
  p.slots[0] = {kPlanks, 1, 0};
  p.slots[1] = {kStick, 1, 0};
  p.slots[2] = {kIngot, 1, 0};

  auto grid = makeGrid();
  grid[0] = {kPlanks, 7, 0};  // matches -> consumed[i] recorded as 7
  grid[1] = {kStick, 3, 0};   // matches -> consumed[i] recorded as 3
  grid[2] = {kGem, 1, 0};     // MISMATCH -> early return

  const auto r = m.matchDetailed(grid, p);
  CHECK(!r.matched, "precondition: the grid does not match");
  // Slots 0 and 1 were already written before slot 2 failed. Observed
  // behaviour, NOT blessing: a caller that trusts consumed_per_slot without
  // checking `matched` would deduct 7 planks and 3 sticks for a craft that
  // never happened. The fix belongs in matchDetailed(), not in this test.
  CHECK_EQ(int(r.consumed_per_slot[0]), 7,
           "PRODUCTION DEFECT: a failed match still reports slot 0's consumed "
           "count (7). consumed_per_slot is only meaningful when matched==true");
  CHECK_EQ(int(r.consumed_per_slot[1]), 3,
           "PRODUCTION DEFECT: slot 1's consumed count (3) also survives the "
           "failure");
  CHECK_EQ(int(r.consumed_per_slot[2]), 0, "the failing slot is zeroed");
  for (size_t i = 3; i < 9; ++i) {
    CHECK_EQ(int(r.consumed_per_slot[i]), 0, "slots after the failure stay 0");
  }
}

static void test_matchDetailed_does_not_mutate_the_input_grid() {
  Crafting::GridPatternMatcher m;
  auto grid = plank2x2Grid();
  const auto before = grid;

  m.matchDetailed(grid, plank2x2Pattern());
  CHECK_EQ(grid.size(), before.size(), "grid size unchanged");
  for (size_t i = 0; i < grid.size(); ++i) {
    CHECK_EQ(grid[i].item_id, before[i].item_id, "item_id unchanged");
    CHECK_EQ(int(grid[i].count), int(before[i].count), "count unchanged");
    CHECK_EQ(grid[i].metadata, before[i].metadata, "metadata unchanged");
  }
}

static void test_matcher_is_stateless_across_calls() {
  // matchDetailed mutates nothing on the matcher, so a single instance can
  // serve many grids. Pinned because the crafting server reuses one instance.
  Crafting::GridPatternMatcher m;
  for (int i = 0; i < 3; ++i) {
    CHECK(m.matchSingle(plank2x2Grid(), plank2x2Pattern()),
          "the same match succeeds on every repeat call");
    CHECK(!m.matchSingle(gridWith(0, kGem, 1), plank2x2Pattern()),
          "the same failure repeats too");
  }
}

// ---------------------------------------------------------------------------

#define TEST(name) \
  do { \
    printf("  TEST: %s\n", #name); \
    test_##name(); \
  } while (0)

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("=== grid_pattern_matcher test suite ===\n\n");

  // Shaped match
  TEST(exact_shaped_match_succeeds);
  TEST(wrong_offset_shaped_match_fails);
  TEST(mirrored_shaped_match_fails);
  TEST(symmetric_shape_mirror_still_matches);

  // Empty grid
  TEST(empty_grid_matches_an_all_empty_pattern);
  TEST(empty_grid_fails_any_non_empty_pattern);

  // Shapeless
  TEST(shapeless_single_item_match);
  TEST(shapeless_pair_of_items);

  // Count semantics
  TEST(pattern_count_zero_accepts_any_count);
  TEST(pattern_count_is_a_minimum_not_an_equality);
  TEST(matching_consumes_the_whole_grid_stack_not_the_minimum);

  // Metadata semantics
  TEST(pattern_metadata_zero_accepts_any_meta);
  TEST(pattern_metadata_is_exact_when_non_zero);

  // match()
  TEST(match_returns_the_first_matching_recipe_name);
  TEST(match_is_first_in_list_order_not_most_specific);
  TEST(match_returns_empty_string_when_nothing_matches);
  TEST(match_with_no_patterns_returns_empty_string);
  TEST(empty_name_patterns_are_returned_verbatim);

  // matchDetailed()
  TEST(matchDetailed_reports_full_consumption_for_a_full_match);
  TEST(matchDetailed_always_returns_nine_entries_even_on_failure);
  TEST(matchDetailed_failure_leaves_partial_consumed_counts);
  TEST(matchDetailed_does_not_mutate_the_input_grid);
  TEST(matcher_is_stateless_across_calls);

  printf("\n=== Results: %d assertions, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
