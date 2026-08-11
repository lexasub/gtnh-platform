# Change: Add Game Modes (Spectator / Creative / Survival)

## Why
The client has a `GameMode` enum and a `/gamemode` console command, but the mode is
**inert**: the camera always flies with no collision, the NEI panel always spawns
items, and break/place is always enabled. The mode never changes behavior. We want the
mode to actually gate movement, NEI spawning, and (at beta) interaction, and to lay the
protocol contract now so the later switch to server-authoritative validation is a
one-gate change.

## Already in Place
- `GameMode` enum in `src/services/game_client/Common/Inventory.h:16` —
  SURVIVAL=0, CREATIVE=1, ADVENTURE=2, SPECTATOR=3 + `GameModeName()`
- `InventoryState::gameMode` (line 60) — client-authoritative mode state
- `ConsoleWindow` (`UI/Windows/player/ConsoleWindow.cpp`) — `/gamemode <0|1|2|3>`
  command, `/help`, extensible `RegisterCommand` registry
- Break/place + inventory consumption in `InteractionSystem`

## What Changes
- **Permission matrix** mapping each `GameMode` to capabilities
  (canFly, noclip, canBreak, canPlace, infiniteItems); mode becomes the single source
  of truth for behavior
- **Mode-aware movement**: SPECTATOR/CREATIVE keep fly + noclip (current behavior);
  SURVIVAL/ADVENTURE use gravity + AABB collision against solid blocks, owned by a
  new `PlayerController` (physics extracted from `Camera::Update`)
- **NEI gating**: item spawning blocked in SURVIVAL and ADVENTURE
  (`GameModePerm::InfiniteItems`)
- **Interaction gating**: break/place currently disabled in ADVENTURE/SPECTATOR;
  the full per-mode interaction semantics (consumption etc.) are tracked by the
  separate `add-interaction-mode-gating` change
- **Protocol**: single `GameModeChange` table (`core.fbs`) with wire constant
  `kGameModeChange = 30`; server stores the per-player mode and echoes it, trusting
  the client for now
- Default mode stays **CREATIVE** (unchanged; SPECTATOR default deferred)

## Impact
- Affected specs: `game-modes` (new capability), `protocol`
- Affected code:
  - `src/protocol/core.fbs` (PlayerMode enum mirroring client GameMode),
    `src/protocol/gateway.fbs` (two new union members), C++ `GatewayMsg` constants
    (gateway.h / NetClient.h)
  - `src/services/game_client/` — new `PlayerController`, Camera (delegate movement),
    NeiPanel (gate), InteractionSystem (consume in survival), GameClient (mode wiring),
    Common/Inventory.h (default constant)
  - `src/services/gateway/` (relay), `src/services/simulation_core/` (per-player mode store)
