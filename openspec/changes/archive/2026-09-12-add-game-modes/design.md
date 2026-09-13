# Design: Add Game Modes

## Context
The client already has a `GameMode` enum (`Common/Inventory.h:16`), a
client-authoritative `InventoryState::gameMode`, and a working `/gamemode 0|1|2|3`
console command (`ConsoleWindow.cpp`). But the mode is inert — nothing reads it:
- `Camera::Update` (Camera.cpp:33) always moves as a free-fly camera: no gravity, no
  collision, `SPEED=14.317`, ascend/descend keys
- `NeiPanel` always spawns items on click (`ActionHandler.SpawnItem`)
- `InteractionSystem` always allows break/place and consumes the selected slot
  regardless of mode
- No protocol message exists to tell the server about the mode

The platform is server-authoritative (gateway/simcore), but in the current dev phase
the server trusts the client.

## Goals / Non-Goals
- Goals:
  - Mode actually gates behavior via a permission matrix (canFly, noclip, canBreak,
    canPlace, infiniteItems)
  - SURVIVAL/ADVENTURE movement: gravity + AABB collision (PlayerController extracted
    from Camera)
  - NEI spawn blocked in SURVIVAL and ADVENTURE
  - Protocol contract (GameModeChange) laid now; server stores + echoes
  - Default mode stays CREATIVE (current dev behavior)
- Non-Goals:
  - Server-authoritative mode validation (beta)
  - Full per-mode break/place semantics (inventory consumption, spectator/creative
    differentiation) — tracked by the separate `add-interaction-mode-gating` change
  - Adventure mode mechanics, tool requirements, fall/void damage, mode persistence
    in MetaDB

## Decisions

### D1: Client-side authority now, protocol laid for beta flip
`InventoryState::gameMode` stays the source of truth (already the case). On switch,
the client applies the mode locally AND sends `GameModeChange`; SimulationCore stores
the per-player mode and echoes it back without permission checks. Beta flip =
one permission check in the server handler; the wire and client behavior already exist.

### D2: Presets + permission matrix, not orthogonal flags
`GameMode` maps to a fixed matrix (canFly, noclip, canBreak, canPlace, infiniteItems).
Simpler to reason about and matches GTNH-style modes. Independent flags can come later
if a mode needs a custom combo.

### D3: PlayerController owns movement
New `PlayerController` (`game_client/Player/PlayerController.cpp`, owned by Camera)
holds position, velocity, and onGround and implements per-mode physics:
- Fly (SPECTATOR/CREATIVE): free-fly, the movement math previously in `Camera::Update`
- Walk (SURVIVAL/ADVENTURE): gravity (25 blocks/s²), jump, sneak, axis-separated AABB
  sweep against solid blocks via `World::GetBlockAt`, ground scan with ~0.5-block
  step-up
`Camera` becomes a pure view: keeps look/orient/frustum, computes movement intent
from bindings, delegates to the controller, and renders from its eye position.
Alternatives considered:
- Branch inside `Camera::Update` — minimal diff, but makes Camera fat and mixes view
  with body physics (rejected)
- No-gravity locked-Y walk — not real survival (rejected)

### D4: Single GameModeChange message
A single `GameModeChange` table (player_id, new_mode) lives in `core.fbs`, reusing the
`GameMode` enum that mirrors the client values (0-3). The wire constant is
`kGameModeChange = 30` (defined in both the C++ `GatewayMsg` namespace and
`NetClient.h`); the gateway relays it on the `player.gamemode.change` /
`player.gamemode.changed` topics. Both the fbs and the C++ `GatewayMsg` constants must
stay in sync — the fbs header comment already documents the known staleness between
them (gateway.fbs:17-30), and the code comments in NetClient.h keep them adjacent to
the other message constants.

### D5: NEI gating client-side only for now
NeiPanel checks the matrix before spawning. Server-side rejection of spawn in SURVIVAL
is deferred to beta (the spawn path lives in SimulationCore/MetaDB handlers; no wire
change needed then).

### D6: Mode reads are hot, switches are cold
Mode switching is a rare user action, but the mode is *read* every frame (movement,
interaction gate, NEI gate). This is fine: a scalar enum compare that branch-predicts
perfectly because the value is stable across frames. No indirection, no locks, no
recomputation on switch. The only genuinely new per-frame cost is the SURVIVAL AABB
collision sweep (3 axes × a few voxel lookups via `World::GetBlockAt`); it stays
axis-separated with early-outs (skip axes with ~zero velocity) and is negligible next
to existing chunk meshing.

On the client the mode lives on the main frame thread (no contention). When the server
becomes authoritative at beta, the per-player mode store SHALL be a
`std::atomic<uint8_t>` or an ECS component on the player entity — written by the
`GameModeChange` handler thread, read by the sim tick thread — and SHALL NOT share a
cache line with hot tick data (false-sharing risk).

## Risks / Trade-offs
- Client-authority cheat window (client can keep spawning items in survival) →
  accepted for dev; closed at beta by the server gate (D1)
- Physics cost: AABB sweep per frame against chunk voxels → single AABB (player),
  axis-separated, negligible vs existing chunk meshing
- Walking into an unloaded chunk → clamp movement to loaded bounds for now; void
  handling later
- Protocol constant drift (known issue) → fbs + C++ constants in the same change +
  frame-parse test

## Migration Plan
- No data migration. The `GameModeChange` table is additive in `core.fbs` (already
  generated). Default mode stays CREATIVE (`Common/Inventory.h:64`).

## Open Questions
- Fall damage and void death in SURVIVAL — now or beta? (default: beta)
- Creative-flight fallback key in SURVIVAL? (default: none)
- Per-player mode persistence in MetaDB — only meaningful when mode becomes
  server-authoritative (beta)
