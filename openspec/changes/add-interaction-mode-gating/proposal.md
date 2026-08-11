# Change: Add Mode-Based Block Interaction Gating

## Why
`add-game-modes` introduced the `GameModePerm` permission matrix (canBreak, canPlace,
canFly, noclip, infiniteItems) and made the game mode actually gate movement and NEI
spawning. Block break/place is currently gated only by a coarse inline check
(`GameClient.cpp:332` disables interaction in ADVENTURE and SPECTATOR), and inventory
consumption on placement is server-side only. This change formalizes the per-mode
interaction contract so the matrix is the single source of truth for block
interaction, with a spec that tests can target.

## What Changes
- **Break/place gating** keyed to `GameModePerm::CanBreak` / `CanPlace` instead of the
  inline ADVENTURE/SPECTATOR check:
  - CREATIVE: break + place allowed, no inventory consumption (infinite items)
  - SURVIVAL: break + place allowed; placement consumes server-side (server-authoritative
    inventory already deducts; client does not locally decrement)
  - ADVENTURE / SPECTATOR: break + place disallowed
- **Client consumption**: none — placement cost is enforced by SimulationCore (already
  server-authoritative). No client-side slot decrement is added.
- **Spec coverage**: interaction behavior per mode captured under `player-interaction`
  with scenarios, plus a unit test over `GameModePerm::CanBreak`/`CanPlace`.

## Impact
- Affected specs: `player-interaction`
- Affected code: `src/services/game_client/GameClient.cpp` (replace inline gate with
  matrix predicates), `Common/Inventory.h` (matrix already present), a
  `GameModePerm` interaction test
- Depends on: `add-game-modes` (GameModePerm)
