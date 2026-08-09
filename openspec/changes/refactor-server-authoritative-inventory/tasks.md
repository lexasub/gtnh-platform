# Implementation checklist — server-authoritative inventory

Verified by a 6-agent workflow + completeness critic (8 agents, ~721K tok, 255 tool calls). Findings G1–G20 / OH1–OH9 folded into the tasks below.

## 0. Step 0 — Build bootstrap (PREREQ for all phases) ✅ DONE

The worktree had **no `cmake-build-debug/`** (gitignored) and **no Go stubs** (`src/protocol/generated/go/`) → MetaDB go build was broken until regenerated. Note: `src/protocol/generated/go/go.mod` is an untracked local file — recreated manually (mirrors main).

- [x] 0.1 `conan install . -of cmake-build-debug --build=missing` (offline, resolved from `~/.conan2` cache).
- [x] 0.2 `cmake -S . -B cmake-build-debug -G Ninja -DCMAKE_TOOLCHAIN_FILE=.../conan_toolchain.cmake`.
- [x] 0.3 meta_db `--go` target → Go stubs materialized in `src/protocol/generated/go/`, MetaDB go build OK.
- [x] 0.4 Full `ninja -j5` + `ctest` baseline: **11/11 green**.

## 1. Phase A — Protocol + server rule table + player-inventory (ATOMIC cutover) ✅ DONE

**OH1/OH9**: D2 is breaking and replaces the handler on the single `player.inventory.actions` topic → no safe partial state. Land server + client together in ONE commit.

- [x] 1.1 `core.fbs`: `InventoryAction` = click descriptor (CLICK/QUICK_MOVE/DROP/DRAG_PLACE/PICKUP_ALL, `button`, `mods`, `container_id`, `slot`, `count`); `InventoryUpdate` + `cursor` + `container_id`/`container_pos`/`container_slots` (D3). Fixed `core.fbs` schema comment (was `3=CRAFT`) + `gateway.h` header comment. Regenerated C++ (simcored/gateway/game_client) + Go (meta_db) stubs.
- [x] 1.2 `PlayerInventoryStore`: per-player `cursor` (`cursors_` map), `getCursor`/`setCursor`, combined `setSlotsAndCursor` (one postMutation → one publish), `buildUpdate` includes cursor + empty container. Bound = `kInventorySlots=40`.
- [x] 1.3 `Storage/InventoryClick.h` (header-only, pure): `ApplyContainerClick` rule table — pick/place/merge/swap/half/place-1/quick-move(hotbar↔main + container-aware)/drag-place/pickup-all/drop, **meta-preserving**, stack-limit 64, cross-container refs via `InventoryRef`.
- [x] 1.4 `test/test_inventory_click.cpp` + registered in `simcored_test` (15 tests). **simcored 65/65, whole suite 11/11.**
- [x] 1.5 Handler: rewrote `Storage/InventoryActionHandler.{h,cpp}` IN PLACE as the click handler (same class name/ctor/wiring → zero CMake/wiring churn). Parses the click descriptor, applies the rule against player slots + cursor (container_id≠0 rejected with a warn until Phase B), persists via `setSlotsAndCursor`. **Removes the old double-publish** (setSlots + explicit pub). Legacy root `simulation_core/InventoryActionHandler.*` is NOT compiled (header-only import for structs) — left for Phase E.
- [x] 1.6 Wiring unchanged: `SimCoreMessageHandler.cpp:84` already dispatches `player.inventory.actions` → Storage `InventoryActionHandler`. `main.cpp` postMutation unchanged (publishes cursor too now).
- [x] 1.7 Client dual-mode:
  - `DragManager`: added click path (`OnPlayerSlotClick`/`OnPlayerDrop`/`OnPlayerDragPlace`, `ClickInfo` + `kClickAction*` constants) with **no local mutation**; legacy `OnSlotActivated` mutation path preserved for unconverted containers.
  - `SlotGridComponent::SetAuthoritative(bool)`: authoritative → click path for activate/Q/RMB-drag-distribute; fixed hover→`UpdateHover` (feeds distribute).
  - `PlayerInventory`: authoritative grids, cursor parse from `InventoryUpdate`, cursor preview at mouse, hotbar-closed click via `OnPlayerSlotClick`.
  - `NetClient::SendInventoryAction` → new 7-arg click signature.
  - `UIManager::SetNetClient`: legacy `SetActionCallback` now machine-drag only (player/grid sends retired); new `SetClickCallback` → `SendInventoryAction` click protocol.
- [x] 1.8 Verify: full build green, **ctest 11/11** (toctou_test flaky under SQPOLL fallback, passes on rerun), gameclientd + simcored_exec link. Manual runtime pass deferred to run.sh (needs router+gateway+simcore+client stack).

## 2. Phase B — Chest as a live container

**OH3/OH8/G2**: chest.open/load request + load-gating MUST precede click handling (else the open race = dup/loss, the exact failure `dataLoaded_` guards today). Fix the **null-`inv_` player-grid deref** (ChestWindow.cpp:108-113) inside the rewrite, not in E. **G6 atomicity** (top risk): the client-trusted `player.chest.save` must be deleted in the SAME commit as the `InventoryActionHandler` container_id click routing.

Plan verified by a 7-agent Phase B research workflow (chest-flow, workbench-open-template, world-container-inventory, ess-client-api, client-chest-window, protocol-gateway + planner). Key findings: no client→server chest.open exists today (load is a server side-effect of right-click via `ChestInteractHandler`); `WorldContainerInventory` is COMPILED-BUT-UNWIRED dead code (not a live third path — do NOT reuse; the live paths are `ChestInteractHandler`+`player.chest.save`, both replaced here); `entity.state.*` router topics + `InventoryIntegration.*` are dead, NOT chest-path (leave them). Free GatewayMsg id 19 (between 18=chest.save and 20).

- [x] **S1 — Protocol + gateway + NetClient**: `kChestOpenReq=19`/`kChestCloseReq=45` in `gateway.h`+`NetClient.h`; `core.fbs` `table ContainerOpenReq{player_id,pos}`; gateway ctrl passthrough; `NetClient::SendChestOpenReq/CloseReq`. Stubs regenerated.
- [x] **S2 — `ContainerSessionRegistry`** (`Storage/ContainerSession.h`, header-only, mutex-guarded): `ContainerSession{x,y,z,entity_type,slots}`, `open/get/find/close`. Includes `kChestEntityType=3`.
- [x] **S3 — `ChestStateManager`** (`Storage/ChestStateManager.{h,cpp}`, in CMake): cache-first `loadSlots`, `saveSlots`, `clearSlots`; `EncodeChestBlob`/`DecodeChestBlob` (MachineState, count uint16↔uint8 clamped). Shared `PublishFullInventory` helper.
- [x] **S4+S5+S6 (atomic)**: `ChestOpenHandler`/`ChestCloseHandler` (Network/, in CMake, wired in `setup()`); `InventoryActionHandler` now takes `chestSessions`+`chestStateManager`, routes container_id=1 via session lookup with OH3 gate, `PublishFullInventory` + live `saveSlots`; deleted inline `player.chest.save` handler + `Subscribe`; deleted `NetClient::SendChestSaveReq` (decl+impl); `ChestInteractHandler` slimmed to ack+OPEN_UI; `ChestWindow` snapshot-driven (kInventoryUpdate container_id==1), sends open/close, `SetPlayerId` via factory in `BlockUIFactory`, `SaveState` removed. `kChestSaveReq=18` left free (not renumbered).
- [x] **S7 — Client container-aware clicks** (fixes G2): `DragManager::OnContainerSlotClick/Drop/DragPlace`; `SlotGridComponent::SetContainerId`; authoritative clicks/Q/RMB route via containerId_; legacy Escape-cancel/drag-preview/dropEnabled guarded `!authoritative_`; `ChestWindow` chest grid `SetAuthoritative(true)`+`SetContainerId(1)`, player grid → `RenderPlayerInventoryGrid(..., true)`, cursor preview rendered.
- [x] **S8 — Reconciliation + verify**: greps confirm old chest paths gone (`player.chest.save`/`kChestSaveReq`/`SendChestSaveReq` → empty); `entity.state.*`/`InventoryIntegration` remain dead (not chest-path, leave); ONE chest persistence path (session → MachineState → ESS :5200). Full build + **ctest 11/11** (toctou passed on rerun).

**Phase B risks (from workflow):** G6 atomicity (top); OH3 open/load race (drop-click-warn + client `dataLoaded_` guard); io-vs-main threading (registry mutex, `getContainer` pointer valid only on main thread — close()/erase main-thread-only); ChestInteractHandler slim must land with the client snapshot branch or chest renders blank; double publish per click is deterministic but ChestWindow MUST filter `container_id!=1`; MachineState count width (uint16↔uint8); per-player chest session = last-write-wins across concurrent players (document, Phase B scope).

## 3. Phase C — Machine as a live container

**OH4/G3/G4**: session MUST write the **same ECS `InventoryContainer`** MachineSystem ticks (not a copy); `ItemFlowHandler` conversion + MachineSystem InventoryUpdate publish hook must land BEFORE the client window switch.

- [ ] 3.1 Machine container session on the SAME ECS `InventoryContainer` (copy chest session WITHOUT destroy-on-close — machine entity carries MachineComponent/RecipeProgress/EnergyStorage).
- [ ] 3.2 **Convert `ItemFlowHandler`** (G3) through the session + fix its synthetic `SetMachineSlotReq` arg bug (`CreateSetMachineSlotReq(fbb,0,&pos_fb,slot_idx,item_id,count,255)` puts 255 into meta, `player_slot=0` → may wipe player 0 slot 0).
- [ ] 3.3 **Add MachineSystem→InventoryUpdate publish hook** (G4): recipe Pass 1 consumes inputs / Pass 2 places outputs + `pushOutputToPipe` today broadcast only via `publishBlockEntityUpdate` — must republish machine slots so the client window doesn't go stale after a craft.
- [ ] 3.4 THEN switch `MachineWindow` to read slots from `InventoryUpdate` (energy/progress stays `BlockEntityUpdate`); remove per-slot `SetMachineSlotReq` movement path, machine drag context, `onMachineSlotAck` flow (G4, BlockUIFactory.cpp:84-91).
- [ ] 3.5 Verify: player↔machine drag + quick-move; recipe still consumes inputs / emits outputs after slot moves; meta preserved (drill damage).

## 4. Phase D — Workbench grid as a live container

**OH5/G9/G10**: decide shared-vs-private grid FIRST; `CraftRequestHandler` grid change + player-inventory deduction removal = ONE atomic change; drop `GridUpdate` only after the snapshot carries grid slots.

- [ ] 4.1 **Decide shared-vs-private grid semantics** (G9): D4 makes the grid world-bound (pos-keyed) → two players opening the same workbench share contents (GTNH deviates from vanilla's return-grid-to-player-on-close). Make the call and document it.
- [ ] 4.2 Grid as container session (9 cells via `WorkbenchStateManager`, which today saves to ESS but **never broadcasts** — G10: add a live publish after each mutation).
- [ ] 4.3 `CraftingGrid` snapshot-driven: replace `HandleActivate` staging + `kGridFlag` with container-click; grid state from snapshot. **CraftRequestHandler reads the session grid AND removes its player-inventory deduction loop atomically** (closes double-deduction/dupe).
- [ ] 4.4 Drop `GridUpdate` message usage (only after container snapshot carries grid slots — don't defer to E, avoids split-brain). Verify put/take (incl. shift-click), preview, craft consumes inputs + returns result.

## 5. Phase E — Cleanup & polish

**G1/G15/G16**: split structs out of the legacy header BEFORE deleting it; delete dead code, don't "fix" it.

- [ ] 5.1 **Split `ItemStack`/`InventorySlot` structs out of legacy `simulation_core/InventoryActionHandler.h`** into a shared header FIRST (imported by `ElectricDrillHandler.cpp`, `ItemEnergyStorage.h`, `test/test_main.cpp`), then delete the legacy handler + its dead publish-on-topic (G1).
- [ ] 5.2 **Delete** `RenderSlotGrid` (dead — zero callers) and the never-wired `SetMachineActionCallback`/`SetMachineSlotAckCallback` (G15); fix the **real** RMB-distribute hover defect: `SlotGridComponent` writes `inv_->dragHoverSlot` (SlotGrid.cpp:224) but `OnRightDragDistribute` reads `dm_->GetHoverSlot()` — wire hover→`UpdateHover` (G14).
- [ ] 5.3 Cursor rendering polish (preview at mouse, tooltip), ESC = place cursor back to origin slot. Update `ActionHandler.cpp:76` drill-in gate + `InteractionSystem.cpp:26-32` (GetHeldItem reads `selectedSlot`, should consider the server cursor — G12).
- [ ] 5.4 Delete `SendChestSaveReq`, per-slot `SetMachineSlotReq` movement, `kGridSlotBase`/`kMachineSlotBase`/`kMachineOutputBase` numbering. Rework client `DragManager_test.cpp` to the click-translator shape. **Add server tests beyond the rule table** (G20): ContainerClickHandler wiring, open/close session lifecycle, machine/workbench session reads.
- [ ] 5.5 Full `ctest` + client build; end-to-end manual pass via `run.sh` (player inventory, chest, machine, workbench); `git push`.

## 6. Spec & validation

- [ ] 6.1 Keep `specs/protocol` and `specs/player-interaction` deltas in sync with implementation (incl. schema comment fix, `container_id` per-player scope).
- [ ] 6.2 `openspec validate refactor-server-authoritative-inventory --strict` passes before and after.

## Known risks tracked

- **OH1/OH9** — Phase A atomic; no compiling-but-broken intermediate.
- **OH2/G11** — DragManager dual-mode until B/C/D convert; mutation retained for unconverted containers.
- **OH6/G17** — Step 0 build bootstrap blocks everything; do it first.
- **G3/G4** — machine: same-ECS-container + ItemFlowHandler + MachineSystem publish hook all before client switch.
- **G9** — workbench shared-vs-private grid decision before D.
