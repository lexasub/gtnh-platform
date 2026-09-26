#pragma once

// ──────────────────────────────────────────────────────────────────────────
// GameMode + the GameModePerm permission matrix — the single source of truth
// for what a game mode allows.
//
// WHY THIS FILE EXISTS (gp-t71g)
// -----------------------------
// It used to live inside src/game/client/Common/Inventory.h, i.e. in a header
// under the CLIENT tree. That made it unreachable from the server without
// simcore including a client header — a layering inversion — which is in turn
// why PlaceBlockHandler had no mode gate at all: the matrix it needed was
// one directory away from being visible to it, so nobody wired it up.
//
// This follows the precedent already set in this repo for exactly this
// problem: src/engine/registry/coords/Coords.h is documented as "Shared
// coordinate types extracted from game_client/Common/Types.h". Shared types
// get a lower layer, and the client header re-exports them.
//
// Inventory.h still includes this, so every existing client translation unit,
// every UI panel and all three gameclient test binaries keep resolving
// GameMode / GameModePerm / TryGameModeFromWire by the same unqualified names
// they always used. Nothing had to change at a single call site.
//
// Layering: src/common/ is the gtnh_wire INTERFACE target, described by its
// own CMakeLists as "Wire-protocol headers shared across apps and game
// layers". It pulls in NOTHING — cstdint only — so the server does not acquire
// a glm/ImGui/bgfx dependency by learning the permission table. That matters:
// gtnh_game_actions links gtnh_wire PUBLIC already, so the whole change needs
// no CMake edit at all.
//
// NOT a second copy. If the matrix changes, this is the one place it changes,
// and the client gate, the InteractionSystem backstop, the NEI gate and the
// server placement gate all follow together.
// ──────────────────────────────────────────────────────────────────────────

#include <cstdint>

// ──────────────────────────────────────────────────────────────────────────
// GameMode — the game mode, client-authored and server-validated
// ──────────────────────────────────────────────────────────────────────────
// The wire declares `enum GameMode : uint8` (src/protocol/core.fbs:25) and
// flatbuffers does not range-check enum values on read, so any of 0..255 can
// arrive. Everything below is written as an allow-list over the four defined
// values so a mode nobody defined fails CLOSED rather than inheriting
// permissions by accident.
enum class GameMode : uint8_t {
  SURVIVAL = 0,
  CREATIVE = 1,
  ADVENTURE = 2,
  SPECTATOR = 3,
};

inline const char* GameModeName(GameMode mode) {
  switch (mode) {
    case GameMode::SURVIVAL:  return "SURVIVAL";
    case GameMode::CREATIVE:  return "CREATIVE";
    case GameMode::ADVENTURE: return "ADVENTURE";
    case GameMode::SPECTATOR: return "SPECTATOR";
  }
  return "UNKNOWN";
}

// Whether `m` is one of the four modes the enum names. Every other byte is
// a value no producer ever intended.
inline bool IsDefinedGameMode(GameMode m) {
  switch (m) {
    case GameMode::SURVIVAL:
    case GameMode::CREATIVE:
    case GameMode::ADVENTURE:
    case GameMode::SPECTATOR:
      return true;
  }
  return false;
}

// Converts a raw byte off the wire into a GameMode, REJECTING anything the
// enum does not name. `out` is written only when this returns true.
//
// This is the boundary validator for `GameMode`, which arrives as a bare
// uint8 in the FlatBuffers schema (`enum GameMode : uint8` in core.fbs:25) and
// is NOT range-checked on read — any of 0..255 can arrive.
//
// House rule, applied here exactly as BlockDrops.cpp:13-30 and
// ItemRegistry.cpp:56-60 apply it to ItemId::pack: do not decide validity
// from the VALUE's shape, decide it from the SET. ItemId::pack scans for
// digits and returns 0 both for a deliberate air id and for a typo, so those
// loaders re-check the literal spelling before trusting a 0. The analogous
// trap here would be a "clamp anything above 3 down to 3" or a "non-zero
// means valid" test, both of which invent a mode the enum does not name and
// both of which fail OPEN — a bogus 255 would silently become SPECTATOR and
// be handed every flight and infinite-item permission. So the check is an
// explicit membership test over the four defined values.
//
// Note the FlatBuffers default: flatc omits a scalar field whose value equals
// the declared default, so a legitimate SURVIVAL arrives as an ABSENT field
// and reads back as 0. "0 is valid" is therefore required, and the rule
// explicitly does not treat an absent field as a missing/invalid mode.
inline bool TryGameModeFromWire(uint8_t raw, GameMode& out) {
  const GameMode candidate = static_cast<GameMode>(raw);
  if (!IsDefinedGameMode(candidate)) return false;
  out = candidate;
  return true;
}

// ──────────────────────────────────────────────────────────────────────────
// GameModePerm — permission matrix, the single source of truth for what a
// mode allows. Values mirror what the client enforces today:
//
//   | Mode      | canFly | noclip | canBreak | canPlace | infiniteItems |
//   |-----------|--------|--------|----------|----------|---------------|
//   | SPECTATOR |   ✅   |    ✅   |    ❌    |    ❌    |      ✅       |
//   | CREATIVE  |   ✅   |    ✅   |    ✅    |    ✅    |      ✅       |
//   | SURVIVAL  |   ❌   |    ❌   |    ✅    |    ✅    |      ❌       |
//   | ADVENTURE |   ❌   |    ❌   |    ❌    |    ❌    |      ❌       |
//
// canFly/noClip: CREATIVE and SPECTATOR fly with no collision (current dev
// behavior; a true creative-vs-spectator noclip split is future work).
// canBreak/canPlace: enforced by GameClient::Update through
// CanInteractWithWorld, and — since gp-t71g — ALSO enforced server-side, by
// PlaceBlockHandler, which is the half that is actually authoritative.
// ──────────────────────────────────────────────────────────────────────────
namespace GameModePerm {
inline bool CanFly(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SPECTATOR;
}
inline bool NoClip(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SPECTATOR;
}
inline bool InfiniteItems(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SPECTATOR;
}
inline bool CanBreak(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SURVIVAL;
}
inline bool CanPlace(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SURVIVAL;
}
// Whether the client may run the world-interaction path (block break and
// place) at all. This is the predicate GameClient::Update uses in place of
// the old inline `gameMode != ADVENTURE && gameMode != SPECTATOR`, which was
// a deny-list and therefore ADMITTED every mode the enum does not define
// (gp-ul16). Deliberately identical to CanBreak && CanPlace rather than a
// third spelling of the same set: the gate covers both actions, and a
// separate copy is a third thing that can drift. The allow-list shape is
// also what makes an out-of-range mode take the safe path, so it fails
// closed even if a future assignment site bypasses TryGameModeFromWire.
inline bool CanInteractWithWorld(GameMode m) {
  return CanBreak(m) && CanPlace(m);
}
// Whether the right-click world-mutation path may run at all (gp-t71g).
//
// This names a decision that used to be spread across the body of
// GameClient::Update's right-click block with no name and no gate. That block
// is NOT one action — it is three distinct capabilities behind a single
// condition:
//
//   1. plain placement, via SendBlockAction(RIGHT_MOUSE_CLICK);
//   2. the wrench WRENCH_CYCLE, via SendToolAction, which rewrites a
//      pipe/cable connection and is a world mutation the matrix names no
//      "canTool" column for;
//   3. the open-UI intent — which is NOT a client-side arm at all. The client
//      sends frame (1) and the SERVER decides between open-UI and place
//      (MachineInteractHandler and ChestInteractHandler both claim
//      RIGHT_MOUSE_CLICK and outrank PlaceBlockHandler in the dispatch tuple).
//
// So the question "gate the whole block, or only the placement arm?" has a
// concrete answer, and it is not "spectators may still open a machine window
// read-only" as the issue assumed. Opening a machine window is NOT read-only:
// MachineOpenHandler registers a ContainerSession, after which
// InventoryActionHandler applies container clicks to the LIVE ECS
// InventoryContainer and persists the result to EntityStateStore. There is no
// mode gate anywhere in that chain, so a spectator with an open window can
// still move items.
//
// Given that, all three capabilities are world mutations and all three are
// denied by exactly the same two modes, so they take exactly the same
// predicate. Splitting the block would buy a narrower gate at the cost of a
// real hole: the wrench arm would stay available to a SPECTATOR by
// right-click while InteractionSystem's own G-key wrench arm is already gated
// on CanInteractWithWorld (InteractionSystem.cpp:150-158) — i.e. the two
// wrench paths would disagree, which is precisely the asymmetry gp-t71g was
// filed about.
//
// Named so the choice is pinned in one testable place rather than re-spelled
// at the call site. It is deliberately CanInteractWithWorld and not CanPlace:
// the arms it covers outnumber the placement, and the call site should not
// have to know which column of the matrix justifies its own gate.
inline bool CanRightClickMutateWorld(GameMode m) {
  return CanInteractWithWorld(m);
}
} // namespace GameModePerm
