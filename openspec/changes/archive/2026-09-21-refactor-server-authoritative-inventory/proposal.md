# Change: Server-authoritative Minecraft-style inventory interaction

## Why

Inventory drag-and-drop in the game client is a **client-optimistic simulation over a single slot vector**. It cannot express cross-inventory movement (chest ↔ player, machine ↔ player, workbench grid ↔ player) and breaks on every multi-step cursor operation:

- **Quick-move (shift/ctrl+ЛКМ) is a no-op on the server** — the client emits `kActionQuickMove=3`, but `Storage/InventoryActionHandler` only handles cases 0/1/2 and warns on 3 (`Storage/InventoryActionHandler.cpp:18-26`). Nothing moves.
- **Server case 0 = full slot SWAP, client semantics = "transfer N items"** — place-1, partial merge and half-drag desync: the server `std::swap`s whole slots, then the next `player.inventory.update` snapshot reverts the client's optimistic view (e.g. a 40+20 merge "splits back" into 20/40).
- **RMB half-pickup targets `slot=255` (the cursor), which the server bounds-checks against the 40 player slots** — `dst>=inv.size()` → no-op. The "take half" gesture does nothing.
- **The server has no cursor.** Pickup sends no message; every subsequent cursor op is un-tracked server-side → permanent drift.
- **Chest is not a live server inventory.** It lives in EntityStateStore as `MachineState`, saved as a whole snapshot only on window close (`SendChestSaveReq`). No per-slot actions, no quick-move target.
- **`SlotGridComponent` never calls `DragManager::UpdateHover()`** inside container windows, so RMB-drag distribute acts on a stale hover slot.
- **`RenderSlotGrid` invokes `OnSlotActivated` twice** when both `clickCb` and `dragMgr` are set.

Result: only single-vector drags inside the player inventory "work", and even those desync on partial operations.

## Status (2026-08-10)

- **Phases A–D implemented & committed on `main`** (squashed: `f8d1e4a feat(inventory): server-authoritative click model`): click-model protocol + server cursor (A), chest (B), machine (C), workbench (D) as live `container_id=1` sessions. Chest/machine/workbench windows are snapshot-driven; `ctest` green (11/11).
- **Workbench deviation from the original plan**: the grid is a world-bound server-synced **staging area** — craft still consumes from the player inventory and `GridUpdate` is retained as the craft-feedback channel (see design D12).
- **Remaining — Phase E cleanup**: delete legacy root `simulation_core/InventoryActionHandler.*` (after splitting structs), server-side `MachineSlotHandler` + gateway `SetMachineSlotReq` route, `RenderSlotGrid`, dead callbacks; RMB-distribute hover fix; cursor/drill-in polish; client test rework; final validation + push.

## What Changes

Adopt Minecraft's **server-authoritative container-click model**:

- **Protocol (`core.fbs`)** ✅ done
  - `InventoryAction` repurposed into a container-click descriptor: `action_type` (CLICK / QUICK_MOVE / DROP / DRAG_PLACE / PICKUP_ALL), `button`, `mods`, `container_id`, `slot`, `count`. **BREAKING** semantics for `action_type`; old `source_slot`/`target_slot`/`meta` removed.
  - `InventoryUpdate` extended with the server-owned **cursor stack** and the **open-container slots** (`container_id` + `container_pos` + `container_slots`), so one snapshot drives player + cursor + container windows.
- **Server (simcore)** ✅ done
  - `PlayerInventoryStore` gained a per-player **cursor slot**.
  - Pure, unit-tested click rule table `Storage/InventoryClick.h` (`ApplyContainerClick`) implements pickup / place / merge / swap / half / place-1 / quick-move / double-click / drop / RMB-drag-distribute across player slots and an open **container session** (chest / machine / workbench).
  - `Storage/InventoryActionHandler` (rewritten in place as the click handler) runs the rule table on authoritative state and publishes a full `player.inventory.update` snapshot after every mutation. Chest / workbench containers persist live (`ChestStateManager` / `WorkbenchStateManager`); machine sessions mutate the **live ECS `InventoryContainer`** in place.
- **Client** ✅ done (dual-mode DragManager; legacy mutation path removed in Phase E)
  - `DragManager` has a click-translator path (no optimistic mutation); cursor is rendered from the server snapshot. Legacy `OnSlotActivated` mutation path retained for unconverted grids until Phase E.
  - `PlayerInventory`, `ChestWindow`, `MachineWindow`, `ClientCraftingWindow` are snapshot-driven; client-side `SendChestSaveReq` and per-slot `SetMachineSlotReq` movement are removed.
  - `SlotGridComponent` hover→`UpdateHover` fixed; `SetAuthoritative(bool)` + `SetContainerId(uint8_t)` route clicks per-window.
- **Gateway** ✅ done — pass-through only: `kChestOpenReq=19`/`kChestCloseReq=45` → `player.chest.open/close`, `kMachineOpenReq=18`/`kMachineCloseReq=46` → `player.machine.open/close`, `kWorkbenchOpenReq=44` → `sim.workbench.load`; `sim.workbench.state` → `kGridUpdate=43` relay retained for craft feedback.

## Impact

- Affected specs: `protocol`, `player-interaction`
- Affected code (implemented):
  - `src/protocol/core.fbs` (+ regenerated stubs)
  - `src/services/simulation_core/Storage/PlayerInventoryStore.{h,cpp}`, `Storage/InventoryActionHandler.{h,cpp}`, new `Storage/InventoryClick.h`, `Storage/ContainerSession.h`, `Storage/ChestStateManager.{h,cpp}`, `Network/ChestOpenHandler/ChestCloseHandler/MachineOpenHandler/MachineCloseHandler/WorkbenchOpenHandler`, `Network/SimCoreMessageHandler.cpp`, `ECS/Reactors/ItemFlowHandler.cpp`, `ECS/Systems/MachineSystem.cpp` (publish hook), `Crafting/CraftRequestHandler.cpp`, `Crafting/WorkbenchStateManager.{h,cpp}`, `Actions/MachineSlotHandler.{h,cpp}` (still wired, Phase E), `main.cpp`
  - `src/services/game_client/UI/Core/DragManager.{h,cpp}`, `UI/Components/SlotGrid.{h,cpp}`, `UI/Components/CraftingGrid.{h,cpp}`, `UI/Components/PlayerInventoryGrid.{h,cpp}`, `UI/Windows/block/ChestWindow.{h,cpp}`, `UI/Windows/block/MachineWindow.{h,cpp}`, `UI/Windows/player/ClientCraftingWindow.{h,cpp}`, `UI/Windows/player/PlayerInventory.{h,cpp}`, `UI/UIManager.cpp`, `Common/Inventory.h`, `Network/NetClient.{h,cpp}`
  - `src/services/gateway/gateway.cpp` (topic wiring for open/close)
  - Tests: `test/test_inventory_click.cpp` (15 rule-table tests) + `test/test_container_click.cpp` (9 container tests); `DragManager_test.cpp` rework pending (Phase E)
- **BREAKING**: wire semantics of `Protocol::InventoryAction.action_type`; removal of `SendChestSaveReq` and client per-slot `SetMachineSlotReq` movement. `kChestSaveReq=18` re-purposed as `kMachineOpenReq`.
