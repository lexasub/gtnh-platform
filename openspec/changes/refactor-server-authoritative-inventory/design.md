# Design: Server-authoritative Minecraft-style inventory interaction

## Context

The client currently simulates drag-and-drop locally (`DragManager` state machine over one `std::vector<ItemStack>& slots`), then best-effort syncs via `Protocol::InventoryAction` with 4 action types (MOVE=full swap, SPLIT=half, DROP=clear, QUICK_MOVE=unhandled). Chests and machines are special-cased parallel paths (`SendChestSaveReq` snapshot-on-close; `SetMachineSlotReq` per-slot moves). The server owns 40 player slots in `PlayerInventoryStore` and pushes full snapshots on `player.inventory.update`.

## Goals / Non-Goals

**Goals**
- All six Minecraft interactions correct across player + chest + machine + workbench-grid: LMB pick/place/merge/swap, RMB half / place-1 / drag-distribute, shift/ctrl+LMB quick-move, double-click pickup-all, Q drop, ESC cancel.
- Server-authoritative state; the client renders snapshots, never mutates authoritative slots.
- No desync between client view and server snapshot (today's core defect).
- Live (per-action) container sync — chests no longer save only on close.

**Non-Goals**
- Fluid/energy management (stays in BlockEntityUpdate / pipe network).
- Inventory slots for armor/offhand/equipment (not modeled yet).
- Middle-click creative give / JEI-style transfer (can be layered later).
- Rewriting MachineSystem recipe logic — only its slot *movement* path is unified.

## Decisions

### D1 — Server owns the cursor (hand) slot
Per player, `PlayerInventoryStore` keeps `cursor: PersistSlot` in addition to the 40 slots. Every click rule runs against (player slots, cursor, container slots). This is what makes half-pickup, place-1, drag-distribute and multi-step drags deterministic — the thing the current protocol structurally cannot do.

- Alternative (rejected): client-owned cursor sent in every action payload. Reduces server work but leaves two sources of truth and every edge case on the client. Rejected for the exact bug class we're fixing.

### D2 — `InventoryAction` becomes a container-click descriptor (breaking)
```
table InventoryAction {
  player_id:uint64;
  action_type:uint8;   // 0=CLICK 1=QUICK_MOVE 2=DROP 3=DRAG_PLACE 4=PICKUP_ALL
  button:uint8;        // 0=LMB 1=RMB
  mods:uint8;          // bit0=shift bit1=ctrl
  container_id:uint8;  // 0=player 1=open container
  slot:uint16;         // slot index inside container_id
  count:uint8;         // used by DRAG_PLACE (1) and validation
}
```
Same topic `player.inventory.actions`; gateway passthrough unchanged. Old `source_slot`/`target_slot`/`meta` fields are removed. Rationale: the server computes the whole operation from `(container, slot, button, mods)` against its own state — exactly Minecraft's `C2SContainerClick`.

### D3 — Single snapshot message drives everything
`InventoryUpdate` gains:
```
cursor: ItemStack;             // server-owned hand
container_id: uint8;           // 0 = none
container_pos: Vec3i;          // block the container is attached to
container_slots: [InventorySlot];
```
Published on `player.inventory.update` after every inventory/container mutation. The client applies it wholesale to `InventoryState` (player slots + cursor + open container). Energy/progress stays on `BlockEntityUpdate`; this message carries *slots only*.

### D4 — Open-container session in simcore
When the client opens a block window, simcore registers a session `player_id → (container_id, container_key, slots)`:
- **Chest**: key = BlockPos(+dim); slots loaded/saved via `EntityStateStore` (`MachineState`, entity_type 3) — reuse the existing chest blob format.
- **Workbench**: key = BlockPos; slots = 9 grid cells; persisted via `WorkbenchStateManager` (EntityStateStore).
- **Machine**: key = BlockPos; slots = the machine's in/out slot arrays, sourced from MachineSystem's container state; writes go back into the machine container so recipe logic stays intact.

One active container per player (`OpenExclusive`). `container.open`/`container.close` topics (or reuse existing open/load messages) register/deregister the session; close persists to EntityStateStore.

`container_id` is **scoped to the per-player session**, not global: `0` = player inventory, `1` = the player's single open container. No global registry or collision management — a `container_id` is only meaningful inside the owning player's session, so two players opening different containers never contend.

### D5 — Pure click rule table, unit-testable
`simcore::applyContainerClick(InventoryState&, ContainerState&, const Click&) → bool changed` lives in a header-only (or static) module with **no I/O**. The handler loads authoritative state, applies the rule, persists if changed, publishes the snapshot. All six interactions and all boundary cases (stack limit 64, empty→non-empty, same-item merge, different-item swap, cross-container move) are covered by direct unit tests — the same granularity as the current `DragManager_test.cpp` but on the server.

### D6 — Client `DragManager` becomes a click translator
`OnSlotActivated(slot, slots, button, shift, ctrl)` → `SendClick(containerId, slot, button, mods, actionType)`; `RenderPreview` draws `InventoryState.cursor`. All optimistic slot mutation, held-item tracking, machine-drag context and grid-flag numbering are deleted. `SlotGridComponent` learns its `container_id` so clicks carry the right container.

### D7 — Keep craft *validation* server-side as today
`CraftRequestHandler` stays the authority for "does this grid craft", consuming inputs and pushing `CraftResponse`. The difference: grid cells are now real container slots (D4), so `CraftingGrid::HandleActivate` staging and the `kGridFlag` external-drag path are replaced by the same container-click flow. `GridUpdate` becomes redundant and is dropped.

### D8 — Phase A is an atomic server+client cutover (verified)
`InventoryAction` semantics are breaking and `ContainerClickHandler` replaces the legacy handler on the single `player.inventory.actions` topic → there is **no safe partial state**. Old client + new server = every op silently no-ops; new client + old server = clicks ignored. Server + client land in ONE commit. The old client "compiles with the send path behind a flag" is insufficient — a compiling-but-broken client is not shippable.

### D9 — DragManager stays dual-mode through the container phases
Chest/machine/workbench windows all depend on DragManager's optimistic mutation for their drags today. Cutting mutation in Phase A would break them before B/C/D convert. So Phase A adds a **click path for the player context** while **retaining the mutation path for unconverted containers**; mutation is deleted in Phase E after all three containers convert. (Verified: DragManager is a genuine optimistic slot-mutator — `OnSlotActivated`/`CancelDrag`/`OnRightDragDistribute` all write the slots vector directly.)

### D10 — Machine session writes the SAME ECS `InventoryContainer` MachineSystem ticks
Machine slot state is authoritative on simcore as an ECS `InventoryContainer` component. The container session must read/write that component in place — **not a copy, not the EntityStateStore snapshot** — or MachineSystem recipe logic desyncs. Also verified: `ItemFlowHandler` is a *second* machine-slot mutator AND a second `player.machine.slot` publisher (with a `SetMachineSlotReq` arg-misalignment bug: `player_slot=0`); it must convert through the session in the same change, and MachineSystem needs an InventoryUpdate publish hook on recipe consume/output or the machine window goes stale post-craft.

### D11 — Chest load must gate clicks
There is **no client→server chest.open/load request today** (load is async after right-click). Phase B adds `chest.open` (template: `kWorkbenchOpenReq→sim.workbench.load→WorkbenchOpenHandler`) and gates clicks until the async `LoadEntityState` completes — preserving the `dataLoaded_` guard semantics — else the first per-action save races the in-flight load and wipes fresh state (item dup/loss).

## Risks / Trade-offs

- **Big change surface** → phased: Step 0 (build bootstrap) → A (atomic) → B → C → D → E. Only A is atomic; B/C/D are each independently testable once A is in.
- **MachineSlotHandler + ItemFlowHandler coupling** → both converted through the container model in Phase C, together, before the client window switches; energy/recipe writes stay in MachineSystem on the same ECS container.
- **Chest save-on-close removal** → live per-action persist; close deregisters. The old `SendChestSaveReq` path **bundles player-inventory persistence** (SimCoreMessageHandler.cpp:207-222) — decoupled in B. Old binary path deleted (E).
- **Workbench grid shared-vs-private** → must be decided before D (D4 pos-keyed grid shares contents across players; GTNH deviates from vanilla's return-grid-on-close). Documented decision required.
- **Two snapshot sources for MachineWindow** (BlockEntityUpdate energy/progress, InventoryUpdate slots) → explicit field ownership; MachineWindow reads slots only from InventoryUpdate.
- **Legacy `simulation_core/InventoryActionHandler.*`** → dead but defines `ItemStack`/`InventorySlot` structs imported by `ElectricDrillHandler.cpp`/`ItemEnergyStorage.h`/`test_main.cpp`; **split the structs into a shared header before deleting** the handler.
- **Build bootstrap** → worktree lacks `cmake-build-debug/` (gitignored) and Go stubs; Step 0 (conan+cmake+ninja+go stubs) must precede all phases or nothing compiles.
- **MachineSlotHandler coupling** → machine *movement* is re-routed through the container model, but energy/recipe writes stay in MachineSystem; D4 keeps the container backed by the machine's real slot arrays so no recipe regression.
- **Chest save-on-close removal** → replaced by live per-action persist in D4; close now only *deregisters*. Old `SendChestSaveReq` binary path deleted (Phase E).
- **Two snapshot sources for MachineWindow** (BlockEntityUpdate for energy/progress, InventoryUpdate for slots) → explicit field ownership documented; MachineWindow reads slots only from InventoryUpdate.
- **Old `simulation_core/InventoryActionHandler.*` (legacy enum-based) is dead code** → removed in Phase E to avoid confusion.

## Migration Plan

0. **Step 0 — Build bootstrap**: conan install + cmake configure (materializes `cmake-build-debug/`, runs all 9 flatc C++ steps) + meta_db `--go` (materializes `src/protocol/generated/go/` so MetaDB builds) + ninja + ctest baseline.
1. **Slice 1 — Phase A, atomic (server + client, player window)**: D2 click descriptor + D3 snapshot (cursor + container fields) + schema-comment fix; PlayerInventoryStore cursor; pure rule table + unit tests (kInventorySlots=40, meta-preserving); ContainerClickHandler + wiring replacing the legacy handler on the topic; client click-translator for the player context + cursor rendering + snapshot-driven player window, DragManager in **dual-mode** (mutation retained for unconverted containers). Ship: player inventory fully server-authoritative, no desync, containers untouched.
2. **Slice 2 — Phase B (chest)**: chest.open/load request + session registration; load-gating before clicks; chest slots through the rule table (container_id=1); ChestWindow snapshot-driven + fix null-`inv_` player-grid deref; decouple bundled player persistence; reconcile WorldContainerInventory + second ESS paths; then remove `SendChestSaveReq`.
3. **Slice 3 — Phase C (machine)**: machine session on the SAME ECS InventoryContainer; convert ItemFlowHandler through the session (fix arg bug); add MachineSystem→InventoryUpdate publish hook; then switch MachineWindow to InventoryUpdate slots; retire MachineSlotHandler movement + per-slot SetMachineSlotReq + machine drag context.
4. **Slice 4 — Phase D (workbench)**: decide shared-vs-private grid; grid as container session (9 cells, WorkbenchStateManager + live publish); CraftingGrid snapshot-driven + drop kGridFlag; CraftRequestHandler reads session grid + removes player-inventory deduction atomically; drop GridUpdate only after the snapshot carries grid slots.
5. **Slice 5 — Phase E (cleanup)**: split ItemStack/InventorySlot structs out of the legacy header first, then delete the legacy handler; delete dead RenderSlotGrid + never-wired callbacks; fix the real RMB-distribute hover defect; delete SendChestSaveReq / per-slot SetMachineSlotReq / grid-flag numbering; rework client tests + add server lifecycle tests; full ctest + run.sh manual pass + git push.

## Open Questions

- Container id assignment: **resolved — per-player session scope** (see D4).
- Should `InventoryUpdate` carry the container, or a separate `ContainerUpdate` message? Design prefers one message (D3); reconsider if BlockEntityUpdate overlap becomes confusing.
- **Workbench shared-vs-private grid** (G9): decision required before Phase D.
- **Request-id correlation** (G13): machine acks route by window lookup today (racy); container open/close + acks need request-id correlation, or a snapshot-only model that needs no ack.
- **Chest entity_type / schema**: reuse the existing `MachineState` entity_type-3 chest blob, or introduce a dedicated chest schema (G7/G8 note two ESS paths that must stay consistent).
- **MachineSystem publish cadence** (G4): publish InventoryUpdate after every recipe consume/output, or only when the machine window is open?
