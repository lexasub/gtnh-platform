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

## What Changes

Adopt Minecraft's **server-authoritative container-click model**:

- **Protocol (`core.fbs`)**
  - Repurpose `InventoryAction` into a container-click descriptor: `action_type` (CLICK / QUICK_MOVE / DROP / DRAG_PLACE / PICKUP_ALL), `button`, `mods`, `container_id`, `slot`, `count`. **BREAKING** semantics for `action_type`.
  - Extend `InventoryUpdate` with the server-owned **cursor stack** and the **open-container slots** (`container_id` + `container_pos` + `slots`), so one snapshot drives player + cursor + container windows.
- **Server (simcore)**
  - `PlayerInventoryStore` gains a per-player **cursor slot**.
  - A pure, unit-testable click rule table `applyContainerClick(state, click)` implements pickup / place / merge / swap / half / place-1 / quick-move / double-click / drop / RMB-drag-distribute, across player slots and an open **container session** (chest / machine / workbench).
  - New `ContainerClickHandler` (replaces `Storage/InventoryActionHandler`) runs the rule table on authoritative state and publishes a full `player.inventory.update` snapshot after every mutation. Chest / workbench containers persist live to EntityStateStore; machine slots stay coupled to MachineSystem.
- **Client**
  - `DragManager` becomes a thin click→action translator (no optimistic mutation, no held-item state); cursor is rendered from the server snapshot.
  - `InventoryState` gains `cursor` + open-container fields.
  - `PlayerInventory`, `ChestWindow`, `MachineWindow`, `CraftingWindow` become snapshot-driven; per-window special cases (`kMachineSlotBase`, `kGridFlag`, `SendChestSaveReq`, per-slot `SetMachineSlotReq`) are removed for slot movement.
  - Fix `SlotGridComponent` hover→`UpdateHover` and `RenderSlotGrid` double-invoke.
- **Gateway**: pass-through only; the container session is registered when the client opens a block window (new `container.open` / `container.close` topic or reuse existing open/close messages).

## Impact

- Affected specs: `protocol`, `player-interaction`
- Affected code:
  - `src/protocol/core.fbs` (+ regenerated stubs)
  - `src/services/simulation_core/Storage/PlayerInventoryStore.{h,cpp}`, `Storage/InventoryActionHandler.{h,cpp}`, new `Storage/ContainerClickHandler.*`, `Network/SimCoreMessageHandler.cpp`, `main.cpp`, `Actions/MachineSlotHandler.{h,cpp}`, `Crafting/CraftRequestHandler.{h,cpp}`, `Storage/WorkbenchStateManager.*`
  - `src/services/game_client/UI/Core/DragManager.{h,cpp}`, `UI/Components/SlotGrid.{h,cpp}`, `UI/Components/CraftingGrid.{h,cpp}`, `UI/Windows/block/ChestWindow.{h,cpp}`, `UI/Windows/block/MachineWindow.{h,cpp}`, `UI/Windows/player/ClientCraftingWindow.{h,cpp}`, `UI/Windows/player/PlayerInventory.{h,cpp}`, `UI/UIManager.cpp`, `Common/Inventory.h`, `Network/NetClient.{h,cpp}`
  - `src/services/gateway/gateway.cpp` (minor topic wiring)
  - Tests: new rule-table tests; rework `DragManager_test.cpp`
- **BREAKING**: wire semantics of `Protocol::InventoryAction.action_type` and removal of `SendChestSaveReq` / per-slot `SetMachineSlotReq` movement.
