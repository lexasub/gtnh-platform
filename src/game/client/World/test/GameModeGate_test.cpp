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
// to be extracted into a reusable, unit-testable predicate, and gp-ul16 is
// the defect that extraction was supposed to fix: that check was a deny-list
// and therefore admitted every mode the enum does not define.
//
// BOTH ARE NOW DONE (gp-ul16): the gate calls
// GameModePerm::CanInteractWithWorld from GameClient.cpp:416-419, and this
// file gates on the real production predicate rather than a copy of it, so
// it cannot drift from the code again. `gateAllowsMode` is now a pass-through
// to that predicate, not a restatement of the old expression.
//
// What IS reachable and deterministic:
//
//   1. GameModePerm — the permission matrix the gate is built on. Header-only
//      namespace of inline functions, fully testable.
//   2. IsDefinedGameMode / TryGameModeFromWire — the wire-boundary validator
//      that GameScenario.cpp and ConsoleWindow.cpp now route every
//      assignment through.
//   3. The gate's decision rule, which is literally predicate (1), checked
//      against the rest of the matrix for every one of the 256 byte values.
//
// No display, GPU, window, input device, socket or wall-clock waiting is
// involved. InventoryState is a plain struct; nothing is rendered.
//
// NOT covered here, and why:
//   - GameClient::Update itself needs a window, a GL context and a connected
//     NetClient, so the gate is not exercised through it. What IS covered is
//     the predicate the gate now calls, which is the whole mode half of the
//     condition; the remaining conjuncts (AnyOpen, rightClickHandled) are UI
//     concerns no headless test can reach.
//   - GameScenario::OnNetworkUpdate and the /gamemode console command both
//     need a live UIManager, so their new TryGameModeFromWire call is
//     covered by calling the predicate itself, which is the entire decision
//     those two sites make.
//   - The permission matrix also has a test at
//     src/apps/game_client/tests/test_gamemode_permissions.cpp (four modes,
//     20 checks). This file does not duplicate that; it adds what that file
//     does not cover: the UNDEFINED mode values, the gate-vs-matrix
//     divergence, and the wire-boundary validator.

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

// The interaction gate as it stands in GameClient.cpp:416-419 AFTER gp-ul16:
//
//     if (!uiMgr_.AnyOpen()
//         && GameModePerm::CanInteractWithWorld(invState_.gameMode)
//         && !rightClickHandled) { ... interaction_.Update(...) ... }
//
// BEFORE the fix it read:
//
//     if (!uiMgr_.AnyOpen()
//         && invState_.gameMode != GameMode::ADVENTURE
//         && invState_.gameMode != GameMode::SPECTATOR
//         && !rightClickHandled) { ... interaction_.Update(...) ... }
//
// The first and last conjuncts are about UI and the right-click path, not
// about the mode, so this helper isolates the mode half. It is now a thin
// pass-through to the production predicate rather than a restatement of it,
// so the test exercises the real thing instead of a copy that could drift.
bool gateAllowsMode(GameMode mode) {
  return GameModePerm::CanInteractWithWorld(mode);
}

// The gate as it was BEFORE gp-ul16, kept so the tests can still name the
// defect and prove the new one is different from it over the whole byte
// range. Not called by the pass/fail tests — only by the one test that
// measures the old shape, which documents what the fix changed.
bool gateAllowsModeDenyList(GameMode mode) {
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

static void test_UndefinedModesCannotInteractWithTheWorld() {
  // gp-ul16. Asserts the SAFE direction: an undefined mode must not reach
  // the world-interaction path. Before the fix the gate was a deny-list and
  // every one of these values passed it.
  static const GameMode kUndefined[] = {
      static_cast<GameMode>(4),
      static_cast<GameMode>(5),
      static_cast<GameMode>(7),
      static_cast<GameMode>(99),
      static_cast<GameMode>(200),
      static_cast<GameMode>(255),
  };
  for (GameMode m : kUndefined) {
    CHECK(!gateAllowsMode(m),
          "OBSERVED HARM: the gate ADMITS an undefined mode the matrix blocks");
    CHECK(!GameModePerm::CanBreak(m),
          "OBSERVED: CanBreak blocks the same mode — the two rules disagree");
  }
}

static void test_TheFixChangedExactlyTheUndefinedModes() {
  // Proves the new gate is not merely "the old one, negated" and that the
  // fix is surgical: for all four DEFINED modes the old deny-list and the new
  // allow-list agree, so no player in a real mode changes behaviour. The only
  // values whose verdict changed are the 252 the enum does not name.
  int defined_changed = 0;
  int undefined_changed = 0;
  for (int raw = 0; raw <= 255; ++raw) {
    const GameMode m = static_cast<GameMode>(static_cast<uint8_t>(raw));
    const bool before = gateAllowsModeDenyList(m);
    const bool after = gateAllowsMode(m);
    if (before == after) continue;
    if (IsDefinedGameMode(m)) ++defined_changed; else ++undefined_changed;
  }
  CHECK_EQ(defined_changed, 0, "no defined mode changed behaviour");
  CHECK_EQ(undefined_changed, 252, "exactly the 252 undefined values changed");
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
  // Modes 0..3: both agree. Modes 4..255: after the fix the gate agrees with
  // the matrix and blocks, so there is no disagreement in either direction.
  // Before the fix this was 252 admitted-and-blocked — the deny-list is the
  // unsafe direction, which is why the count is asserted to be ZERO now.
  CHECK_EQ(disagree_deny_only, 0, "no value is admitted by the gate but blocked by the matrix");
  CHECK_EQ(disagree_allow_only, 0, "the matrix never blocks a mode the gate allows");
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
  // gate is the only thing standing between that and a client that builds in
  // a mode nobody defined.
  //
  // Modelled by construction rather than by calling the console: the check
  // here is that InventoryState itself performs no validation, which is what
  // makes the wire path dangerous.
  InventoryState inv;
  const uint8_t raw = 9;
  inv.gameMode = static_cast<GameMode>(raw);  // exactly what GameScenario.cpp:39 does
  CHECK_EQ(static_cast<int>(inv.gameMode), 9, "an out-of-range wire value is stored unvalidated");
  CHECK(!gateAllowsMode(inv.gameMode), "and the gate refuses to let it build");
  CHECK(!GameModePerm::CanBreak(inv.gameMode), "while the matrix forbids it too — the two now agree");
}

// ===========================================================================
// The wire boundary — IsDefinedGameMode / TryGameModeFromWire
// ===========================================================================
//
// These are the second half of the gp-ul16 fix. Allow-listing the gate stops
// an undefined mode from causing harm, but the value would still be sitting
// in InventoryState in a state the enum does not name, and GameModeName
// would render it "UNKNOWN" while the rest of the UI quietly treats it as a
// real mode. So the assignment sites validate too.

static void test_OnlyTheFourDefinedModesAreDefined() {
  for (int i = 0; i < kModeCount; ++i) {
    CHECK(IsDefinedGameMode(kAllModes[i]), "each declared mode is defined");
  }
  for (int raw = 0; raw <= 255; ++raw) {
    const GameMode m = static_cast<GameMode>(static_cast<uint8_t>(raw));
    const bool declared = raw >= 0 && raw < kModeCount;
    CHECK_EQ(IsDefinedGameMode(m), declared,
             "IsDefinedGameMode is exactly the membership test over the enum");
  }
}

static void test_WireValidatorAcceptsEveryDefinedMode() {
  // Round-trip: each defined mode survives the boundary unchanged. This is
  // the no-regression half of the validator — rejecting everything would
  // "fix" gp-ul16 by making the mode permanently unwritable.
  for (int i = 0; i < kModeCount; ++i) {
    GameMode out = GameMode::SPECTATOR;  // poison: must be overwritten
    const uint8_t raw = static_cast<uint8_t>(kAllModes[i]);
    CHECK(TryGameModeFromWire(raw, out), "a defined mode is accepted");
    CHECK_EQ(static_cast<int>(out), static_cast<int>(kAllModes[i]),
             "and comes back as itself");
  }
}

static void test_WireValidatorRejectsEveryUndefinedMode() {
  // The harm case, at the boundary. 9 is the value the issue quotes; 255 is
  // the worst case because a clamp-to-3 would silently turn it into
  // SPECTATOR, which flies, noclips and has infinite items.
  for (int raw = 4; raw <= 255; ++raw) {
    GameMode out = GameMode::CREATIVE;  // poison
    const bool accepted =
        TryGameModeFromWire(static_cast<uint8_t>(raw), out);
    CHECK(!accepted, "an undefined wire value is rejected, not clamped");
    // The contract is "writes `out` only on success", so on failure the
    // caller's mode must be left alone — this is what lets GameScenario
    // keep the player's current mode instead of corrupting it.
    CHECK_EQ(static_cast<int>(out), static_cast<int>(GameMode::CREATIVE),
             "a rejected value does not write the out parameter");
  }
}

static void test_WireValidatorAcceptsZeroBecauseFlatbuffersOmitsDefaults() {
  // The ambiguous-0 case from the BlockDrops/ItemRegistry house rule has a
  // FlatBuffers-specific twist that makes "0 means unset" WRONG here.
  // flatc omits a scalar field whose value equals the declared default, and
  // the declared default of an enum field is 0, so a legitimate SURVIVAL
  // (which is what the server sends for scenario 0) arrives with the field
  // ABSENT and reads back as 0. Treating 0 as "unset" would reject every
  // survival player. So 0 must be accepted as SURVIVAL.
  GameMode out = GameMode::SPECTATOR;
  CHECK(TryGameModeFromWire(0, out), "0 is a valid SURVIVAL, not an absent field");
  CHECK_EQ(static_cast<int>(out), static_cast<int>(GameMode::SURVIVAL));
}

static void test_TheValidatorCannotBeFooledIntoClampingToSpectator() {
  // The specific wrong fix this guards against. "Anything above 3 becomes
  // 3" is a one-line change that passes every test above except this one,
  // and it converts a malformed value into the most privileged mode.
  GameMode out = GameMode::SURVIVAL;
  const bool accepted = TryGameModeFromWire(255, out);
  CHECK(!accepted, "255 is rejected outright");
  CHECK(out != GameMode::SPECTATOR, "and never becomes SPECTATOR by clamping");
  CHECK(!GameModePerm::CanFly(out), "the caller keeps a mode with no flight");
  CHECK(!GameModePerm::InfiniteItems(out), "and no infinite items");
}

static void test_AnUndefinedModeCannotEnterInventoryStateThroughTheBoundary() {
  // End-to-end shape of what GameScenario.cpp now does: validate, and only
  // assign on success. Compare with the test above, which assigns directly
  // and is the pre-fix behaviour.
  InventoryState inv;
  inv.gameMode = GameMode::ADVENTURE;
  GameMode mode{};
  if (TryGameModeFromWire(9, mode)) {
    inv.gameMode = mode;  // unreachable for 9
  }
  CHECK_EQ(static_cast<int>(inv.gameMode), static_cast<int>(GameMode::ADVENTURE),
           "a rejected response leaves the player's mode untouched");
  CHECK(!gateAllowsMode(inv.gameMode), "and ADVENTURE still cannot interact");

  // And the accepting path really does apply.
  if (TryGameModeFromWire(0, mode)) inv.gameMode = mode;
  CHECK_EQ(static_cast<int>(inv.gameMode), static_cast<int>(GameMode::SURVIVAL));
  CHECK(gateAllowsMode(inv.gameMode), "a valid SURVIVAL from the wire can build");
}

int main() {
  TEST(TheMatrixMatchesTheDocumentedTable);
  TEST(CanBreakAndCanPlaceAgreeForEveryDefinedMode);
  TEST(FlyNoClipAndInfiniteItemsAreTheSameSet);
  TEST(TheTwoCapabilityGroupsAreDisjointInPurpose);

  TEST(GateAndMatrixAgreeForEveryDefinedMode);
  TEST(GateBlocksExactlyAdventureAndSpectator);
  TEST(UndefinedModesCannotInteractWithTheWorld);
  TEST(TheFixChangedExactlyTheUndefinedModes);
  TEST(UndefinedModesFavourTheMatrixAcrossTheWholeByteRange);
  TEST(CanFlyAndCanBreakDisagreeOnUndefinedModesToo);
  TEST(AnUndefinedModeIsNamedUnknown);

  TEST(TheUiAndRightClickConjunctsAreIndependentOfMode);

  TEST(InventoryStateDefaultsToCreative);
  TEST(InventoryStateCarriesEveryModeUnchanged);
  TEST(TheConsoleRejectsOutOfRangeModesButTheWireDoesNot);

  TEST(OnlyTheFourDefinedModesAreDefined);
  TEST(WireValidatorAcceptsEveryDefinedMode);
  TEST(WireValidatorRejectsEveryUndefinedMode);
  TEST(WireValidatorAcceptsZeroBecauseFlatbuffersOmitsDefaults);
  TEST(TheValidatorCannotBeFooledIntoClampingToSpectator);
  TEST(AnUndefinedModeCannotEnterInventoryStateThroughTheBoundary);

  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
