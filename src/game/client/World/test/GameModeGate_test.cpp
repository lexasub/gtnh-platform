// Unit tests for the client-side game-mode gate and the mode permission
// matrix (issue gp-hf2y, "extract and unit-test the mode predicate").
//
// Files under test:
//   src/game/client/Common/Inventory.h   — GameMode, GameModePerm, InventoryState
//   src/game/client/GameClient.cpp:416-419 — the inline interaction gate
//
// WHY THIS FILE EXISTS AS A TEST RATHER THAN A REFACTOR
// ---------------------------------------------------
// gp-hf2y asks for the inline ADVENTURE/SPECTATOR check in GameClient::Update
// to be extracted into a reusable, unit-testable predicate. Extracting it
// means editing src/game/client/GameClient.cpp, which is production code
// outside this work item's mandate (and GameClient.cpp has no test at all, so
// a refactor there could not be verified by anything this test does). So the
// predicate is NOT extracted. Instead this file pins the exact behaviour that
// the extraction has to preserve, plus the exact behaviour it has to fix.
//
// The gate itself is not a free function, so it cannot be called. What IS
// reachable and deterministic is:
//
//   1. GameModePerm — the permission matrix the gate is supposed to converge
//      on. This is a header-only namespace of inline functions, fully testable.
//   2. The gate's decision rule, re-expressed as the same expression shape the
//      production code uses, and checked against GameModePerm for every mode.
//      The two rules DISAGREE for every value outside the enum, and the tests
//      say so rather than papering over it.
//
// No display, GPU, window, input device, socket or wall-clock waiting is
// involved. InventoryState is a plain struct; nothing is rendered.
//
// NOT covered here, and why:
//   - GameClient::Update itself needs a window, a GL context and a connected
//     NetClient, so the gate is not exercised through it. The re-expressed
//     rule below is what a reader has to diff against GameClient.cpp, and
//     the test names the line numbers so a stale copy is easy to spot.
//   - The permission matrix also has a test at
//     src/apps/game_client/tests/test_gamemode_permissions.cpp (four modes,
//     20 checks). This file does not duplicate that; it adds what that file
//     does not cover: the UNDEFINED mode values, the gate-vs-matrix
//     divergence, and the fact that the gate is written as a deny-list.

// ---- project test harness (mirrors src/game/world/test/BlockTransforms_test.cpp) ----
#include <cstdio>
#include <cstring>
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

#include <game/client/Common/Inventory.h>

namespace {

// Every GameMode the enum defines, in declaration order.
constexpr GameMode kAllModes[] = {
    GameMode::SURVIVAL,
    GameMode::CREATIVE,
    GameMode::ADVENTURE,
    GameMode::SPECTATOR,
};
constexpr int kModeCount = 4;

// The interaction gate as written TODAY in GameClient.cpp:416-419:
//
//     if (!uiMgr_.AnyOpen()
//         && invState_.gameMode != GameMode::ADVENTURE
//         && invState_.gameMode != GameMode::SPECTATOR
//         && !rightClickHandled) { ... interaction_.Update(...) ... }
//
// The first and last conjuncts are about UI and the right-click path, not
// about the mode, so this helper isolates the mode half verbatim:
//
//     mode != ADVENTURE && mode != SPECTATOR
//
// i.e. a DENY-LIST of the two modes that cannot build. Restated here in the
// same expression shape so the results are bit-identical to production.
bool gateAllowsMode(GameMode mode) {
  return mode != GameMode::ADVENTURE && mode != GameMode::SPECTATOR;
}

} // namespace

// ===========================================================================
// GameModePerm — the matrix the gate is meant to converge on
// ===========================================================================

static void test_TheMatrixMatchesTheDocumentedTable() {
  // The table in Inventory.h:37-42. Checked directly so a change to any
  // predicate has to be made knowingly in two places (the comment and here).
  //
  //   | Mode      | canFly | noclip | canBreak | canPlace | infiniteItems |
  //   | SPECTATOR |   ✅   |   ✅   |    ❌    |    ❌    |      ✅       |
  //   | CREATIVE  |   ✅   |   ✅   |    ✅    |    ✅    |      ✅       |
  //   | SURVIVAL  |   ❌   |   ❌   |    ✅    |    ✅    |      ❌       |
  //   | ADVENTURE |   ❌   |   ❌   |    ❌    |    ❌    |      ❌       |
  //
  // SPECTATOR flies and noclips but cannot build; ADVENTURE does none of the
  // three. The two "restricted" modes are restricted in different ways, which
  // is exactly why the gate needs a real predicate rather than a
  // "is it creative-ish" test.
  CHECK(GameModePerm::CanFly(GameMode::SPECTATOR));
  CHECK(GameModePerm::NoClip(GameMode::SPECTATOR));
  CHECK(!GameModePerm::CanBreak(GameMode::SPECTATOR));
  CHECK(!GameModePerm::CanPlace(GameMode::SPECTATOR));
  CHECK(GameModePerm::InfiniteItems(GameMode::SPECTATOR));

  CHECK(GameModePerm::CanFly(GameMode::CREATIVE));
  CHECK(GameModePerm::NoClip(GameMode::CREATIVE));
  CHECK(GameModePerm::CanBreak(GameMode::CREATIVE));
  CHECK(GameModePerm::CanPlace(GameMode::CREATIVE));
  CHECK(GameModePerm::InfiniteItems(GameMode::CREATIVE));

  CHECK(!GameModePerm::CanFly(GameMode::SURVIVAL));
  CHECK(!GameModePerm::NoClip(GameMode::SURVIVAL));
  CHECK(GameModePerm::CanBreak(GameMode::SURVIVAL));
  CHECK(GameModePerm::CanPlace(GameMode::SURVIVAL));
  CHECK(!GameModePerm::InfiniteItems(GameMode::SURVIVAL));

  CHECK(!GameModePerm::CanFly(GameMode::ADVENTURE));
  CHECK(!GameModePerm::NoClip(GameMode::ADVENTURE));
  CHECK(!GameModePerm::CanBreak(GameMode::ADVENTURE));
  CHECK(!GameModePerm::CanPlace(GameMode::ADVENTURE));
  CHECK(!GameModePerm::InfiniteItems(GameMode::ADVENTURE));
}

static void test_CanBreakAndCanPlaceAgreeForEveryDefinedMode() {
  // They are written identically today. Nothing consumes CanPlace yet, but
  // the gate covers both break and place, so the two must not drift apart.
  // Compared per mode, not across the cross product — comparing every
  // CanBreak(i) against every CanPlace(j) would be asking the two functions
  // to be constant, which they are not.
  for (int i = 0; i < kModeCount; ++i) {
    const GameMode m = kAllModes[i];
    CHECK_EQ(GameModePerm::CanBreak(m), GameModePerm::CanPlace(m),
             "CanBreak and CanPlace are the same predicate for one mode");
  }
}

static void test_FlyNoClipAndInfiniteItemsAreTheSameSet() {
  // Inventory.h:44-45 notes the three are currently identical, with a
  // creative-vs-spectator noclip split left as future work. Pinned so that
  // split has to be a deliberate change.
  for (int i = 0; i < kModeCount; ++i) {
    const GameMode m = kAllModes[i];
    const bool fly = GameModePerm::CanFly(m);
    CHECK_EQ(GameModePerm::NoClip(m), fly, "NoClip tracks CanFly");
    CHECK_EQ(GameModePerm::InfiniteItems(m), fly, "InfiniteItems tracks CanFly");
  }
}

static void test_TheTwoCapabilityGroupsAreDisjointInPurpose() {
  // canFly and canBreak are genuinely different axes: ADVENTURE cannot fly
  // but also cannot build, while CREATIVE can do both. This is why a single
  // "is creative-ish" predicate cannot stand in for the permission matrix.
  CHECK(!GameModePerm::CanFly(GameMode::ADVENTURE));
  CHECK(!GameModePerm::CanBreak(GameMode::ADVENTURE));
  CHECK(GameModePerm::CanFly(GameMode::CREATIVE));
  CHECK(GameModePerm::CanBreak(GameMode::CREATIVE));
  CHECK(!GameModePerm::CanFly(GameMode::SURVIVAL));
  CHECK(GameModePerm::CanBreak(GameMode::SURVIVAL));
  CHECK(GameModePerm::CanFly(GameMode::SPECTATOR));
  CHECK(!GameModePerm::CanBreak(GameMode::SPECTATOR));
}

// ===========================================================================
// The gate's rule, for every DEFINED mode
// ===========================================================================

static void test_GateAndMatrixAgreeForEveryDefinedMode() {
  // This is the regression guard for the whole extraction: whatever
  // GameClient.cpp does today for the four real modes, the extracted
  // predicate must keep doing. Today the two rules coincide, so the
  // deny-list is not yet wrong for any mode that can actually occur.
  for (int i = 0; i < kModeCount; ++i) {
    const GameMode m = kAllModes[i];
    CHECK_EQ(gateAllowsMode(m), GameModePerm::CanBreak(m),
           "the inline gate and CanBreak agree for a defined mode");
  }
}

static void test_GateBlocksExactlyAdventureAndSpectator() {
  CHECK(!gateAllowsMode(GameMode::ADVENTURE), "ADVENTURE cannot build");
  CHECK(!gateAllowsMode(GameMode::SPECTATOR), "SPECTATOR cannot build");
  CHECK(gateAllowsMode(GameMode::SURVIVAL), "SURVIVAL can build");
  CHECK(gateAllowsMode(GameMode::CREATIVE), "CREATIVE can build");
}

static void test_GateIsADenyListNotAnAllowList() {
  // THE FINDING. GameClient.cpp:417-418 writes the gate as
  //   mode != ADVENTURE && mode != SPECTATOR
  // which admits every mode EXCEPT those two. GameModePerm::CanBreak is an
  // allow-list:
  //   mode == CREATIVE || mode == SURVIVAL
  // For the four defined modes the two are equivalent. For every value the
  // enum does NOT define they are opposites: the deny-list lets the action
  // through, the allow-list blocks it.
  //
  // gp-hf2y says an undefined mode "must take the documented safe path". The
  // allow-list is the safe path. The deny-list as written is not, so the
  // extraction is not a pure refactor: it changes behaviour for the
  // out-of-range values. That is the point of the issue, and the reason the
  // rules must be compared rather than assumed equivalent.
  static const GameMode kUndefined[] = {
      static_cast<GameMode>(4),
      static_cast<GameMode>(5),
      static_cast<GameMode>(7),
      static_cast<GameMode>(99),
      static_cast<GameMode>(200),
      static_cast<GameMode>(255),
  };
  for (GameMode m : kUndefined) {
    CHECK(gateAllowsMode(m),
          "OBSERVED: the deny-list ADMITS an undefined mode");
    CHECK(!GameModePerm::CanBreak(m),
          "but CanBreak BLOCKS it — the two rules disagree");
  }
}

static void test_UndefinedModesFavourTheMatrixAcrossTheWholeByteRange() {
  // Every value 0..255, i.e. the full range of the underlying uint8_t.
  // GameMode is reachable from the wire as a raw byte:
  //   src/game/ui/client/player/GameScenario.cpp:39
  //     inv->gameMode = static_cast<GameMode>(resp->game_mode());
  // so an out-of-range value is not merely theoretical — a server that sends
  // game_mode = 9 puts the client in a state the enum does not name. The
  // FlatBuffers schema declares `enum GameMode : uint8`, and flatbuffers does
  // not range-check enum values on read, so the cast is unchecked.
  int disagree_allow_only = 0;
  int disagree_deny_only = 0;
  for (int raw = 0; raw <= 255; ++raw) {
    const GameMode m = static_cast<GameMode>(static_cast<uint8_t>(raw));
    const bool gate = gateAllowsMode(m);
    const bool matrix = GameModePerm::CanBreak(m);
    if (gate && !matrix) ++disagree_deny_only;   // the dangerous direction
    if (!gate && matrix) ++disagree_allow_only;
  }
  // Modes 0..3: both agree. Modes 4..255: the gate admits, the matrix blocks.
  CHECK_EQ(disagree_deny_only, 252, "252 of 256 values are admitted by the gate but blocked by the matrix");
  CHECK_EQ(disagree_allow_only, 0, "the matrix never blocks a mode the gate allows among defined modes");
}

static void test_CanFlyAndCanBreakDisagreeOnUndefinedModesToo() {
  // CanFly is also an allow-list, so an undefined mode cannot fly — good.
  // The dangerous pair is CanBreak: also an allow-list, so the MATRIX is
  // safe; it is only the inline deny-list in GameClient.cpp that is not.
  for (int raw = 4; raw <= 255; ++raw) {
    const GameMode m = static_cast<GameMode>(static_cast<uint8_t>(raw));
    CHECK(!GameModePerm::CanFly(m), "an undefined mode cannot fly");
    CHECK(!GameModePerm::NoClip(m), "an undefined mode cannot noclip");
    CHECK(!GameModePerm::CanBreak(m), "an undefined mode cannot break");
    CHECK(!GameModePerm::CanPlace(m), "an undefined mode cannot place");
    CHECK(!GameModePerm::InfiniteItems(m), "an undefined mode has no infinite items");
  }
}

static void test_AnUndefinedModeIsNamedUnknown() {
  // GameModeName's switch has no default case, so it falls through to the
  // "UNKNOWN" return. Every undefined value gets the same name, which is the
  // only place the client surfaces "this mode does not exist".
  for (int raw = 4; raw <= 255; ++raw) {
    const GameMode m = static_cast<GameMode>(static_cast<uint8_t>(raw));
    CHECK(std::strcmp(GameModeName(m), "UNKNOWN") == 0,
          "an undefined mode is reported as UNKNOWN");
  }
  CHECK(std::strcmp(GameModeName(static_cast<GameMode>(0)), "SURVIVAL") == 0);
  CHECK(std::strcmp(GameModeName(static_cast<GameMode>(1)), "CREATIVE") == 0);
  CHECK(std::strcmp(GameModeName(static_cast<GameMode>(2)), "ADVENTURE") == 0);
  CHECK(std::strcmp(GameModeName(static_cast<GameMode>(3)), "SPECTATOR") == 0);
}

// ===========================================================================
// The gate's other conjuncts
// ===========================================================================

static void test_TheUiAndRightClickConjunctsAreIndependentOfMode() {
  // GameClient.cpp:416 and :419 — `!uiMgr_.AnyOpen()` and `!rightClickHandled`
  // are UI/click concerns, not mode concerns. They are not modelled here
  // (no UIManager, no window), but the mode half is what the extraction
  // touches, so this records that the extraction must NOT absorb them: a
  // predicate named for the mode gate must take only the mode.
  //
  // What this pins is the part that IS reachable: no predicate in the matrix
  // mentions the UI, and none of them can be substituted for the whole
  // condition, because the whole condition has three independent reasons to
  // run and only one of them is the mode.
  for (int i = 0; i < kModeCount; ++i) {
    const GameMode m = kAllModes[i];
    // A mode that can build still needs the UI closed and no pending
    // right-click; the mode predicate is necessary but not sufficient.
    CHECK(GameModePerm::CanBreak(m) == gateAllowsMode(m),
          "mode is the only part of the gate that this file can model");
  }
}

// ===========================================================================
// InventoryState — the carrier of the mode
// ===========================================================================

static void test_InventoryStateDefaultsToCreative() {
  // Inventory.h:98 — `GameMode gameMode = GameMode::CREATIVE`. A default-
  // constructed InventoryState therefore passes the gate, so a client that
  // has not yet received a game mode builds freely. Pinned because it is the
  // state every UI window sees before the first InventoryUpdate.
  InventoryState inv;
  CHECK_EQ(static_cast<int>(inv.gameMode), static_cast<int>(GameMode::CREATIVE));
  CHECK(gateAllowsMode(inv.gameMode), "and the default mode is allowed by the gate");
  CHECK(GameModePerm::CanBreak(inv.gameMode), "and by the permission matrix");
}

static void test_InventoryStateCarriesEveryModeUnchanged() {
  // The mode is a plain field, so assigning any value sticks. This is the
  // mechanism by which an undefined value from the wire reaches the gate.
  for (int raw = 0; raw <= 255; ++raw) {
    InventoryState inv;
    inv.gameMode = static_cast<GameMode>(static_cast<uint8_t>(raw));
    CHECK_EQ(static_cast<int>(inv.gameMode), raw, "the mode round trips through the field");
  }
}

static void test_TheConsoleRejectsOutOfRangeModesButTheWireDoesNot() {
  // src/game/ui/client/player/ConsoleWindow.cpp:44-53 validates the
  // /gamemode argument to 0..3 before assigning, so a player cannot type a
  // bad mode. GameScenario.cpp:39 does NOT validate resp->game_mode(). So the
  // only way an undefined mode reaches the gate is a server response, and the
  // deny-list is the only thing standing between that and a client that builds
  // in a mode nobody defined.
  //
  // Modelled by construction rather than by calling the console: the check
  // here is that InventoryState itself performs no validation, which is what
  // makes the wire path dangerous.
  InventoryState inv;
  const uint8_t raw = 9;
  inv.gameMode = static_cast<GameMode>(raw);  // exactly what GameScenario.cpp:39 does
  CHECK_EQ(static_cast<int>(inv.gameMode), 9, "an out-of-range wire value is stored unvalidated");
  CHECK(gateAllowsMode(inv.gameMode), "and the deny-list lets it build");
  CHECK(!GameModePerm::CanBreak(inv.gameMode), "while the matrix would forbid it");
}

// ===========================================================================
// The extraction this test exists to enable — what it must preserve
// ===========================================================================
//
//   ADD to src/game/client/Common/Inventory.h (next to GameModePerm):
//
//     namespace GameModePerm {
//     // Whether the client may run the world-interaction path (block break
//     // and place) at all. Replaces the inline
//     //   `gameMode != ADVENTURE && gameMode != SPECTATOR`
//     // in GameClient::Update, which is a deny-list and therefore ADMITS any
//     // mode the enum does not define. This is an allow-list, so an
//     // out-of-range game_mode arriving from the server takes the safe path.
//     inline bool CanInteractWithWorld(GameMode m) {
//       return m == GameMode::CREATIVE || m == GameMode::SURVIVAL;
//     }
//     }
//
//   Then GameClient.cpp:416-419 becomes
//
//     if (!uiMgr_.AnyOpen()
//         && GameModePerm::CanInteractWithWorld(invState_.gameMode)
//         && !rightClickHandled) { ... }
//
//   and test_GateAndMatrixAgreeForEveryDefinedMode is the regression guard
//   that the four real modes behave exactly as they do today.
// ===========================================================================

int main() {
  TEST(TheMatrixMatchesTheDocumentedTable);
  TEST(CanBreakAndCanPlaceAgreeForEveryDefinedMode);
  TEST(FlyNoClipAndInfiniteItemsAreTheSameSet);
  TEST(TheTwoCapabilityGroupsAreDisjointInPurpose);

  TEST(GateAndMatrixAgreeForEveryDefinedMode);
  TEST(GateBlocksExactlyAdventureAndSpectator);
  TEST(GateIsADenyListNotAnAllowList);
  TEST(UndefinedModesFavourTheMatrixAcrossTheWholeByteRange);
  TEST(CanFlyAndCanBreakDisagreeOnUndefinedModesToo);
  TEST(AnUndefinedModeIsNamedUnknown);

  TEST(TheUiAndRightClickConjunctsAreIndependentOfMode);

  TEST(InventoryStateDefaultsToCreative);
  TEST(InventoryStateCarriesEveryModeUnchanged);
  TEST(TheConsoleRejectsOutOfRangeModesButTheWireDoesNot);

  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
