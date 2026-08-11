# Implementation checklist — server-authoritative inventory

Verified by a 6-agent workflow + completeness critic (8 agents, ~721K tok, 255 tool calls). Findings G1–G20 / OH1–OH9 folded into the tasks below.

**Status (2026-08-10)**: Phases A–D **implemented and committed on `main`** (squashed `f8d1e4a feat(inventory): server-authoritative click model`). Phase E (cleanup) + spec finalization remain. See deviations in 3.4 and 4.3/4.4.

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
- [x] 1.6 Wiring unchanged: `SimCoreMessageHandler.cpp` dispatches `player.inventory.actions` → Storage `InventoryActionHandler`. `main.cpp` postMutation unchanged (publishes cursor too now).
- [x] 1.7 Client dual-mode:
  - `DragManager`: added click path (`OnPlayerSlotClick`/`OnPlayerDrop`/`OnPlayerDragPlace`, `ClickInfo` + `kClickAction*` constants) with **no local mutation**; legacy `OnSlotActivated` mutation path preserved for unconverted containers.
  - `SlotGridComponent::SetAuthoritative(bool)` + `SetContainerId(uint8_t)`: authoritative → click path for activate/Q/RMB-drag-distribute; fixed hover→`UpdateHover` (feeds distribute).
  - `PlayerInventory`: authoritative grids, cursor parse from `InventoryUpdate`, cursor preview at mouse, hotbar-closed click via `OnPlayerSlotClick`.
  - `NetClient::SendInventoryAction` → new 7-arg click signature.
  - `UIManager::SetNetClient`: legacy `SetActionCallback` now machine-drag only (player/grid sends retired); new `SetClickCallback` → `SendInventoryAction` click protocol.
- [x] 1.8 Verify: full build green, **ctest 11/11** (toctou_test flaky under SQPOLL fallback, passes on rerun), gameclientd + simcored_exec link. Manual runtime pass deferred to run.sh (needs router+gateway+simcore+client stack).

## 2. Phase B — Chest as a live container ✅ DONE

**OH3/OH8/G2**: chest.open/load request + load-gating MUST precede click handling (else the open race = dup/loss, the exact failure `dataLoaded_` guards today). Fix the **null-`inv_` player-grid deref** (ChestWindow.cpp:108-113) inside the rewrite, not in E. **G6 atomicity** (top risk): the client-trusted `player.chest.save` MUST be deleted in the SAME commit as the `InventoryActionHandler` container_id click routing.

Plan verified by a 7-agent Phase B research workflow (chest-flow, workbench-open-template, world-container-inventory, ess-client-api, client-chest-window, protocol-gateway + planner). Key findings: no client→server chest.open exists today (load is a server side-effect of right-click via `ChestInteractHandler`); `WorldContainerInventory` is COMPILED-BUT-UNWIRED dead code (not a live third path — do NOT reuse; the live paths are `ChestInteractHandler`+`player.chest.save`, both replaced here); `entity.state.*` router topics + `InventoryIntegration.*` are dead, NOT chest-path (leave them). Free GatewayMsg id 19 (between 18=chest.save and 20).

- [x] **S1 — Protocol + gateway + NetClient**: `kChestOpenReq=19`/`kChestCloseReq=45` in `gateway.h`+`NetClient.h`; `core.fbs` `table ContainerOpenReq{player_id,pos}`; gateway ctrl passthrough (`kChestOpenReq`→`player.chest.open`, `kChestCloseReq`→`player.chest.close`); `NetClient::SendChestOpenReq/CloseReq`. Stubs regenerated.
- [x] **S2 — `ContainerSessionRegistry`** (`Storage/ContainerSession.h`, header-only, mutex-guarded): `ContainerSession{x,y,z,entity_type,slots}` + `Kind` (Chest/Machine/Workbench), `open/get/find/close`, `forEachOpenAt`. Includes `kChestEntityType=3`.
- [x] **S3 — `ChestStateManager`** (`Storage/ChestStateManager.{h,cpp}`, in CMake): cache-first `loadSlots`, `saveSlots`, `clearSlots`; `EncodeChestBlob`/`DecodeChestBlob` (MachineState, count uint16↔uint8 clamped). Shared `PublishFullInventory` helper.
- [x] **S4+S5+S6 (atomic)**: `ChestOpenHandler`/`ChestCloseHandler` (Network/, in CMake, wired in `setup()`); `InventoryActionHandler` now takes `chestSessions`+`chestStateManager`, routes container_id=1 via session lookup with OH3 gate, `PublishFullInventory` + live `saveSlots`; deleted inline `player.chest.save` handler + `Subscribe`; deleted `NetClient::SendChestSaveReq` (decl+impl); `ChestInteractHandler` slimmed to ack+OPEN_UI; `ChestWindow` snapshot-driven (kInventoryUpdate container_id==1), sends open/close, `SetPlayerId` via factory in `BlockUIFactory`, `SaveState` removed. `kChestSaveReq=18` re-purposed as `kMachineOpenReq` (not renumbered).
- [x] **S7 — Client container-aware clicks** (fixes G2): `DragManager::OnContainerSlotClick/Drop/DragPlace`; `SlotGridComponent::SetContainerId`; authoritative clicks/Q/RMB route via containerId_; legacy Escape-cancel/drag-preview/dropEnabled guarded `!authoritative_`; `ChestWindow` chest grid `SetAuthoritative(true)`+`SetContainerId(1)`, player grid → `RenderPlayerInventoryGrid(..., true)`, cursor preview rendered.
- [x] **S8 — Reconciliation + verify**: greps confirm old chest paths gone (`player.chest.save`/`kChestSaveReq`/`SendChestSaveReq` → only stale comments); `entity.state.*`/`InventoryIntegration` remain dead (not chest-path, leave); ONE chest persistence path (session → MachineState → ESS :5200). Full build + **ctest 11/11** (toctou passed on rerun). `test/test_container_click.cpp` added (9 tests: player↔container, quick-move, drop, empty-session drop, sized session, registry open/find/close).

**Phase B risks (from workflow):** G6 atomicity (done); OH3 open/load race (drop-click-warn + session registered before async load); io-vs-main threading (registry mutex, `getContainer` pointer valid only on main thread — close()/erase main-thread-only); ChestInteractHandler slim landed with the client snapshot branch; double publish per click is deterministic but ChestWindow MUST filter `container_id!=1`; MachineState count width (uint16↔uint8); per-player chest session = last-write-wins across concurrent players (documented, Phase B scope).

## 3. Phase C — Machine as a live container ✅ DONE

**OH4/G3/G4**: session MUST write the **same ECS `InventoryContainer`** MachineSystem ticks (not a copy); `ItemFlowHandler` conversion + MachineSystem InventoryUpdate publish hook MUST land BEFORE the client window switch.

- [x] 3.1 Machine container session on the SAME ECS `InventoryContainer`: `ContainerSession::Kind::Machine` — `slotsRef()` re-resolves `reg.try_get<InventoryContainer>()` on EVERY access (EnTT packed storage can relocate components across ticks; block-clear removes them; cached pointers dangle). Layout compatibility enforced by `static_assert` (`InventorySlot` ≡ `PersistSlot`, byte-identical). Session does NOT destroy the machine on close — the machine entity keeps `MachineComponent`/`RecipeProgress`/`EnergyStorage`.
- [x] 3.2 **`ItemFlowHandler` converted** (G3): delivers produced items into open machine sessions via `sessions_->forEachOpenAt(x,y,z,...)`; the synthetic `CreateSetMachineSlotReq` path (255-in-meta / `player_slot=0` wipe bug) is gone.
- [x] 3.3 **MachineSystem→InventoryUpdate publish hook** (G4): `PublishFullInventory` (MachineSystem.cpp:439) after recipe consume/output so the machine window doesn't go stale post-craft.
- [x] 3.4 `MachineWindow` reads slots from `kInventoryUpdate` (energy/progress stays `BlockEntityUpdate`); client machine-drag context + `onMachineSlotAck` + per-slot `SetMachineSlotReq` movement REMOVED (`UIManager.cpp:21` — dead, comment only). **Deviation**: server-side `MachineSlotHandler` + `player.machine.slot` topic + gateway `SetMachineSlotReq` ctrl route (gateway.cpp:525) STILL WIRED — client never sends it; deletion deferred to Phase E 5.3.
- [x] 3.5 Verify: player↔machine drag + quick-move; recipe still consumes inputs / emits outputs after slot moves; quest detection on machine output slot (`checkMachineOutput`, S5b — snapshot output before click, credit on take); meta preserved (drill damage).

## 4. Phase D — Workbench grid as a live container ✅ DONE (deviations in 4.3/4.4)

**OH5/G9/G10**: grid semantics decided FIRST; `CraftRequestHandler` grid read + player-inventory deduction = ONE atomic change; `GridUpdate` **retained** as the craft-feedback channel (deviation, see 4.4).

- [x] 4.1 **Shared-vs-private grid decided** (G9): **world-bound (pos-keyed) staging** via `WorkbenchStateManager` — contents are shared across players opening the same workbench (GTNH-style, deviates from vanilla return-grid-on-close). Craft still consumes from the **player inventory**, so the shared grid is a server-synced staging area, not an item-holding container.
- [x] 4.2 Grid as container session: `WorkbenchOpenHandler` registers `container_id=1` session (kind=Workbench) **IMMEDIATELY** (before async `getGridState`), fills the 9 cells from the load callback (empty-until-loaded guarded); live publish after each mutation via the container-snapshot path (`PublishFullInventory`) + `WorkbenchStateManager::setGridState` persist (G10 — no longer persist-only).
- [x] 4.3 `CraftingGrid` snapshot-driven: `HandleActivate` staging + `kGridFlag` gone (zero refs in `CraftingGrid.cpp`); grid clicks route via `SetAuthoritative(true)` + `SetContainerId(1)`. **`CraftRequestHandler` reads the grid from the server** (`getGridState` — client-supplied slots ignored). **Deviation**: the player-inventory deduction loop was RETAINED (grid = staging; items physically live in the player inventory). Double-deduction/dupe risk is still closed — the grid read is now server-authoritative, so the client can no longer inject slots into the craft.
- [x] 4.4 **`GridUpdate` retained** (`sim.workbench.state` → `kGridUpdate=43`, gateway.cpp:401; CraftRequestHandler still publishes the consumed grid after craft; ClientCraftingWindow applies it position-guarded alongside the container snapshot). Deviation from plan: it stays the craft-result feedback channel — the container snapshot ALSO carries grid slots, but GridUpdate was not dropped. Verify: put/take (incl. shift-click), preview, craft consumes inputs + returns result — green in manual run.sh flow.

## 5. Phase E — Cleanup & polish

**G1/G15/G16**: split structs out of the legacy header BEFORE deleting it; delete dead code, don't "fix" it. New from Phases C/D: retire the server-side `MachineSlotHandler` + gateway `SetMachineSlotReq` route (client path already dead since 3.4).

- [ ] 5.1 **Split `ItemStack`/`InventorySlot` structs out of legacy `simulation_core/InventoryActionHandler.h`** (not compiled since Phase A; imported by `ElectricDrillHandler.cpp`, `ItemEnergyStorage.h`, `test/test_main.cpp`) into a shared header FIRST, then delete the legacy handler + its dead publish-on-topic (G1).
- [ ] 5.2 **Delete** `RenderSlotGrid` (dead — zero callers) and the never-wired `SetMachineActionCallback`/`SetMachineSlotAckCallback` (G15); fix the **real** RMB-distribute hover defect: `SlotGridComponent` writes `inv_->dragHoverSlot` (SlotGrid.cpp:224) but `OnRightDragDistribute` reads `dm_->GetHoverSlot()` — wire hover→`UpdateHover` (G14).
- [ ] 5.3 **Delete the server-side `MachineSlotHandler`** (`Actions/MachineSlotHandler.{h,cpp}`) + the `player.machine.slot` topic registration (SimCoreMessageHandler.cpp:96) + the gateway `SetMachineSlotReq` ctrl route (gateway.cpp:525) + generated `SetMachineSlotReq` usage — client path retired in 3.4. Cursor rendering polish (preview at mouse, tooltip), ESC = place cursor back to origin slot. Update `ActionHandler.cpp:76` drill-in gate + `InteractionSystem.cpp:26-32` (GetHeldItem reads `selectedSlot`, should consider the server cursor — G12).
- [ ] 5.4 Delete `kGridSlotBase`/`kMachineSlotBase`/`kMachineOutputBase` numbering. Rework client `DragManager_test.cpp` to the click-translator shape. **Add server tests beyond the rule table** (G20): ContainerClickHandler wiring, open/close session lifecycle, machine/workbench session reads.
- [ ] 5.5 Full `ctest` + client build; end-to-end manual pass via `run.sh` (player inventory, chest, machine, workbench); `git push`.

## 6. Spec & validation

- [x] 6.1 `specs/protocol` and `specs/player-interaction` deltas updated to implemented reality (chest + workbench open/close, workbench staging semantics, `container_id` per-player scope) — done 2026-08-10.
- [ ] 6.2 `openspec validate refactor-server-authoritative-inventory --strict` passes (re-run before archive).

## Known risks tracked

- **OH1/OH9** — Phase A landed atomic (squashed `f8d1e4a`); no compiling-but-broken intermediate. ✅ resolved
- **OH2/G11** — DragManager dual-mode STILL active (legacy mutation path for unconverted grids); deletion is Phase E 5.2/5.4.
- **G3/G4** — machine: same-ECS-container + ItemFlowHandler + MachineSystem publish hook — all landed with Phase C. ✅ resolved
- **G9** — workbench grid semantics decided: world-bound shared staging + player-inventory consumption (4.1). ✅ resolved
- **G6** — `player.chest.save` deletion landed with the container_id click routing in Phase B (S4–S6). ✅ resolved
- **G10** — WorkbenchStateManager live publish: via container-snapshot path + GridUpdate. ✅ resolved
- **G14** — real RMB-distribute hover defect NOT yet fixed (SlotGrid writes `dragHoverSlot`, distribute reads `dm_->GetHoverSlot()`) → Phase E 5.2.
- **MachineSlotHandler** — server-side legacy per-slot path still wired (`player.machine.slot` + gateway route) → Phase E 5.3.
