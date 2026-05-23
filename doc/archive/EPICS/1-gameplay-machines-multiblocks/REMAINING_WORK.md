# REMAINING_WORK.md

## Completed (Archived)

### Architecture & Core
- ECS MachineSystem (20Hz tick, recipe matching, energy consumption, progress)
- Multiblock detection (matchElectrolyser, registerController)
- BlockEntityUpdate protocol — FlatBuffers table with hatches/covers/fluids
- ConditionEvaluator MachineState — populated from ECS via RecipeManager.cpp

### Components
- `MachineComponent` — core machine data, recipe matching, energy consumption
- `RecipeProgress` — crafting progress tracking
- `InventoryContainer` — machine slots (fixed for MVP)
- `EnergyStorage` — energy buffer

### Resolved Open Questions
- **SpatialIndex** → L2
- **Recipe auto-selection** → UI (GTNH), L2
- **Configurable slots** → fixed for MVP
- **NBT data** → post-MVP

## Remaining

### L3 — Gameplay & Integration

| # | Task | Priority |
|---|------|----------|
| 1 | **Server registration** — `SetBlockAction` → SimulationCore → create ECS entity with `MachineComponent` | High |
| 2 | **Machine GUI** — client handler for `BlockEntityUpdate` (progress bar, energy, slots) | High |
| 3 | **Multiblocks L2** — full gameplay logic (forming, breaking, persistence via `EntityStateStore`) | High |

### Architecture Notes
- `SimulationCore` is **not** a proxy — `RecipeManager`, `EntityStateStore`, `PipeNetwork` are first-class MessageRouter peers
- SpatialIndex deferred to L2
- Multiblock persistence: anchor → `EntityStateStore` RPC → LMDB