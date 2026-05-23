# Remaining Work — 3-Infrastructure-Automation

This document lists work completed and archived, and work pending in the continuation spec.

## Completed and Archived

### Sections A-D, G (Steps 1-6)

| Section | Status | Evidence |
|---------|--------|----------|
| **A.1 EntityStateStore** | ✅ Done | Separate C++ service, LMDB backend, TCP RPC on port 5200. See sibling service docs. |
| **A.2 RecipeManager** | ✅ Done | C++ service loading JSON recipes from `data/recipes/`, CheckRecipe/Craft RPCs. See sibling service docs. |
| **A.3 MetaDB** | ✅ Done | Go/SQLite service for player data, connected to MessageRouter. |
| **B Protocol** | ✅ Done | BlockEntityUpdate table with hatches/covers in core.fbs (see sibling service docs). |
| **C SimulationCore** | ✅ Done | InventoryActionHandler, MachineSystem tick (20 Hz), TileEntity tick system. See sibling service docs. |
| **D Client GUI** | ✅ Done | All ImGui windows implemented per basic-mechanics spec. See sibling service docs. |
| **G Steps 1-6** | ✅ Done | EntityStore, RecipeManager, MetaDB, Protocol, SimulationCore, Client implemented in order. |

## Pending / In Continuation Spec

| Section | Status | Notes |
|---------|--------|-------|
| **E WorldGenerator — ores** | 🟡 Pending | Simple sinusoidal vein generation for test machines (furnace, macerator). Which ores? Open. |
| **F Automation (deferred)** | ⏸️ Deferred | ItemTransporter/hooplers, AE2-like networks, redstone — preserved as architectural notes, not implemented. |

## Open Questions

| Q# | Status | Resolution |
|----|--------|------------|
| Q1 | ✅ Resolved | EntityStateStore is separate service (confirmed in Q4). |
| Q2 | ✅ Resolved | BlockChanged routed via ChunkEventHandler (confirmed in Q4). |
| Q3 | ⏸️ Deferred | ImGui sync frequency — deferred to 100+ machines milestone. Preserved in continuation spec. |
| Q4 | 🟡 Open | Which ores generate? Still needs answer. Preserved in continuation spec. |

## Architecture Notes

**EntityStateStore and RecipeManager infrastructure is now done** through sibling services:
- `doc/EPICS/5-entitystate-store/` — C++ LMDB service
- `doc/EPICS/6-recipe-manager/` — C++ JSON recipe loader
- `doc/EPICS/7-metadb/` — Go SQLite service

These are no longer part of this epic; they are independent Layer 0 services.

## Next Steps

See `../3-infrastructure-automation/3-infrastructure-automation.md` for the continuation spec.
