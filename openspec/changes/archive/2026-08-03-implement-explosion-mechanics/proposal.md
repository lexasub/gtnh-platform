# Change: Implement Explosion on Critical Overheat

## Why
Machines that remain at CRITICAL overheat must eventually explode. This was previously specified inside `implement-heat-management`, but the current implementation is broken: `HeatTransferSystem` Pass 2 re-emplaces `OverheatComponent` every tick (`HeatTransferSystem.cpp:125`), resetting `ticks_at_critical` to 0 — so `ExplosionSystem` (which runs first in the tick order, `main.cpp:82`) never sees the counter reach `EXPLOSION_DELAY_TICKS = 60`. Extracting it here lets the fix and the requirement be tracked independently.

## What Changes
- Fix the counter reset so `OverheatComponent.ticks_at_critical` accumulates across consecutive CRITICAL ticks.
- Machine destroys its block (set to air via `publishBlockChangedEvent`) after 60 consecutive CRITICAL ticks.
- ECS entity is removed on explosion.
- **(BREAKING for `implement-heat-management`):** the `Explosion on Critical Overheat` requirement is removed from that change's spec.

## Impact
- Affected specs: explosion-mechanics (new)
- Affected code:
  - `src/services/simulation_core/ECS/Systems/HeatTransferSystem.cpp` — Pass 2 must preserve `ticks_at_critical` (emplace only if absent; otherwise update `.state` in place)
  - `src/services/simulation_core/ECS/Systems/ExplosionSystem.cpp` — verify behavior
  - `src/services/simulation_core/ECS/components/OverheatComponent.h` — unchanged
- `ExplosionSystem` is already registered in `simulation_core/main.cpp:82`.
