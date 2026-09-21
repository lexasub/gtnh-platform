# Tasks: refactor-server-authoritative-inventory verification

## 5. Phase E — Cleanup & polish

**G1/G15/G16**: split structs out of the legacy header BEFORE deleting it; delete dead code, don't "fix" it. New from Phases C/D: retire the server-side `MachineSlotHandler` + gateway `SetMachineSlotReq` route (client path already dead since 3.4).

- [ ] 5.1 **Split `ItemStack`/`InventorySlot` structs out of legacy `simulation_core/InventoryActionHandler.h`** (not compiled since Phase A; imported by `ElectricDrillHandler.cpp`, `ItemEnergyStorage.h`, `test/test_main.cpp`) into a shared header FIRST, then delete the legacy handler + its dead publish-on-topic (G1).
- [ ] 5.2 **Delete** `RenderSlotGrid` (dead — zero callers) and the never-wired `SetMachineActionCallback`/`SetMachineSlotAckCallback` (G15); fix the **real** RMB-distribute hover defect: `SlotGridComponent` writes `inv_->dragHoverSlot` (SlotGrid.cpp:224) but `OnRightDragDistribute` reads `dm_->GetHoverSlot()` — wire hover→`UpdateHover` (G14).
- [ ] 5.3 **Delete the server-side `MachineSlotHandler`** (`Actions/MachineSlotHandler.{h,cpp}`) + the `player.machine.slot` topic registration (SimCoreMessageHandler.cpp:96) + the gateway `SetMachineSlotReq` ctrl route (gateway.cpp:525) + generated `SetMachineSlotReq` usage — client path retired in 3.4. Cursor rendering polish (preview at mouse, tooltip), ESC = place cursor back to origin slot. Update `ActionHandler.cpp:76` drill-in gate + `InteractionSystem.cpp:26-32` (GetHeldItem reads `selectedSlot`, should consider the server cursor — G12).
- [ ] 5.4 Delete `kGridSlotBase`/`kMachineSlotBase`/`kMachineOutputBase` numbering. Rework client `DragManager_test.cpp` to the click-translator shape. **Add server tests beyond the rule table** (G20): ContainerClickHandler wiring, open/close session lifecycle, machine/workbench session reads.
- [ ] 5.5 Full `ctest` + client build; end-to-end manual pass via `run.sh` (player inventory, chest, machine, workbench); `git push`.

## 6. Spec & validation

- [ ] 6.2 `openspec validate refactor-server-authoritative-inventory --strict` passes (re-run before archive).

## Verification 2026-09-21

Phase E tasks inspected in `src/game/storage`, `src/apps/simcore`, `src/apps/gateway`.

- 5.1 **pending**: `ItemStack`/`InventorySlot` still defined in `src/apps/simcore/InventoryActionHandler.h` (lines 22-36) and included by `src/game/actions/handTool/ElectricDrillHandler.cpp`, `src/game/machines/ItemEnergyStorage.h`, `test/test_main.cpp`. No shared header extracted.
- 5.2 **pending**: `RenderSlotGrid` still present in `src/game/ui/client/components/SlotGrid.h:37` and `SlotGrid.cpp:79`; zero callers outside definition. `SetMachineActionCallback` remains in `src/game/ui/client/core/DragManager.h:149` with no callers. RMB-distribute hover defect persists: `SlotGrid.cpp:236` writes `inv_->dragHoverSlot`, `SlotGrid.cpp:392` reads `dm_->GetHoverSlot()`.
- 5.3 **pending**: `MachineSlotHandler` still compiled: `src/game/actions/MachineSlotHandler.h`, `src/game/actions/MachineSlotHandler.cpp`. Topic registered in `src/apps/simcore/Network/SimCoreMessageHandler.cpp:140`. Gateway ctrl route for `SetMachineSlotReq` still in `src/apps/gateway/gateway.cpp:587-588`. Protocol definition remains in `protocol/core.fbs:524`.
- 5.4 **partial**: `kGridSlotBase` still defined in `src/game/ui/client/core/DragManager.h:92` and used in `src/game/ui/client/UIManager.cpp:28`. `kMachineSlotBase`/`kMachineOutputBase` not found (assumed removed). DragManager_test rework and server tests beyond rule table not observed.
- 5.5 **pending**: no evidence of full `ctest` + client build + manual `run.sh` pass recorded post Phase E.
- 6.2 **done**: `openspec validate refactor-server-authoritative-inventory --strict` passes (2026-09-21).
