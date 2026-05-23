# REMAINING WORK — 0-foundation-item-inventory

## ✅ Done (archived)

| Section | Status | Evidence |
|---------|--------|----------|
| ItemStack protocol (`core.fbs`) | ✅ Done | `table ItemStack { item_id: uint16; count: uint8; meta: uint16; }` in `src/protocol/core.fbs` |
| Inventory architecture | ✅ Done | Types defined: Player (MetaDB), Machine/Workbench (EntityStateStore), Chest (EntityStateStore deferred) |
| EntityStateStore C++ service | ✅ Done | `src/services/entity_state_store/` — 11 files, LMDB, TCP :5200, MessageRouter pub/sub `entity.state.get/set` |
| MetaDB player joined/left | ✅ Done | Gateway publishes `player.joined`/`player.left` → MetaDB `handlePlayerJoined`/`handlePlayerLeft` |
| Protocol schemas | ✅ Done | `entity_state_store.fbs`, `tile_entity_store.fbs` |

## 🔄 2 Remaining Tasks — ✅ DONE (2026-06-20)

1. **Server-authoritative grid state via EntityStateStore** ✅ — `WorkbenchStateManager` rewritten: serializes 9-slot grid ↔ 45-byte blob, persists via `EntityStateStoreClient::SaveEntityState`/`LoadEntityState` (Crafting static library, `src/services/simulation_core/Crafting/`).

2. **Full inventory consumption via MetaDB** ✅ — In `simulation_core/main.cpp`, after `recipe->craft(grid)`, consumed items are deducted from `g_inventories[playerId]`, persisted to file, and `player.inventory.update` published through MessageRouter.

**Continuation spec** (`doc/EPICS/0-foundation-item-inventory/`) — archived. All tasks complete.
