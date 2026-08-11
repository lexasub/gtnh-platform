# Design: Server-authoritative Minecraft-style inventory interaction

## Context

The client currently simulates drag-and-drop locally (`DragManager` state machine over one `std::vector<ItemStack>& slots`), then best-effort syncs via `Protocol::InventoryAction` with 4 action types (MOVE=full swap, SPLIT=half, DROP=clear, QUICK_MOVE=unhandled). Chests and machines are special-cased parallel paths (`SendChestSaveReq` snapshot-on-close; `SetMachineSlotReq` per-slot moves). The server owns 40 player slots in `PlayerInventoryStore` and pushes full snapshots on `player.inventory.update`.

**Status (2026-08-10)**: D1–D11 implemented and committed on `main` (`f8d1e4a`, squashed). D7/D9 deviated (see annotations); D12 documents the workbench staging decision made during Phase D. Phase E (cleanup) remains.

## Goals / Non-Goals

**Goals**
- All six Minecraft interactions correct across player + chest + machine + workbench-grid: LMB pick/place/merge/swap, RMB half / place-1 / drag-distribute, shift/ctrl+LMB quick-move, double-click pickup-all, Q drop, ESC cancel. ✅
- Server-authoritative state; the client renders snapshots, never mutates authoritative slots. ✅ (legacy mutation path pending deletion, Phase E)
- No desync between client view and server snapshot (today's core defect). ✅
- Live (per-action) container sync — chests no longer save only on close. ✅

**Non-Goals**
- Fluid/energy management (stays in BlockEntityUpdate / pipe network).
- Inventory slots for armor/offhand/equipment (not modeled yet).
- Middle-click creative give / JEI-style transfer (can be layered later).
- Rewriting MachineSystem recipe logic — only its slot *movement* path is unified.

## Decisions

### D1 — Server owns the cursor (hand) slot ✅ implemented
Per player, `PlayerInventoryStore` keeps `cursor: PersistSlot` in addition to the 40 slots. Every click rule runs against (player slots, cursor, container slots). This is what makes half-pickup, place-1, drag-distribute and multi-step drags deterministic — the thing the current protocol structurally cannot do.

- Alternative (rejected): client-owned cursor sent in every action payload. Reduces server work but leaves two sources of truth and every edge case on the client. Rejected for the exact bug class we're fixing.

### D2 — `InventoryAction` becomes a container-click descriptor (breaking) ✅ implemented
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

### D3 — Single snapshot message drives everything ✅ implemented
`InventoryUpdate` gains:
```
cursor: ItemStack;             // server-owned hand
container_id: uint8;           // 0 = none
container_pos: Vec3i;          // block the container is attached to
container_slots: [InventorySlot];
```
Published on `player.inventory.update` after every inventory/container mutation. The client applies it wholesale to `InventoryState` (player slots + cursor + open container). Energy/progress stays on `BlockEntityUpdate`; this message carries *slots only*.

### D4 — Open-container session in simcore ✅ implemented
When the client opens a block window, simcore registers a session `player_id → (container_id, container_key, slots)` (`ContainerSessionRegistry`, `ContainerSession::Kind` = Chest/Machine/Workbench):
- **Chest**: key = BlockPos; slots loaded/saved via `ChestStateManager` → EntityStateStore (`MachineState`, `kChestEntityType=3`) — reuses the existing chest blob format.
- **Workbench**: key = BlockPos; slots = 9 grid cells; persisted via `WorkbenchStateManager` (cache + ESS). ⚠️ see D12 — the grid is server-synced **staging**, not an item-holding container.
- **Machine**: key = BlockPos; slots = the machine's in/out slot arrays **in place** on the live ECS `InventoryContainer` (D10); writes go back into the machine container so recipe logic stays intact.

One active container per player (`OpenExclusive`). Open/close topics (`player.chest.open/close`, `player.machine.open/close`, `sim.workbench.load`) register/deregister the session; per-action persist on every mutation; close persists + deregisters.

`container_id` is **scoped to the per-player session**, not global: `0` = player inventory, `1` = the player's single open container. No global registry or collision management — a `container_id` is only meaningful inside the owning player's session, so two players opening different containers never contend.

### D5 — Pure click rule table, unit-testable ✅ implemented
`simcore::Storage/InventoryClick.h` — header-only, pure `ApplyContainerClick(InventoryRef&, PersistSlot& cursor, const ContainerClick&) → bool changed`, **no I/O**. The handler loads authoritative state, applies the rule, persists if changed, publishes the snapshot. All six interactions and boundary cases (stack limit 64, empty→non-empty, same-item merge, different-item swap, cross-container move) are covered by direct unit tests: `test/test_inventory_click.cpp` (15 tests) + `test/test_container_click.cpp` (9 container tests).

### D6 — Client `DragManager` becomes a click translator ✅ implemented (dual-mode)
`OnPlayerSlotClick(slot, button, shift, ctrl)` / `OnContainerSlotClick(slot, containerId, button, shift, ctrl)` / `OnPlayerDrop` / `OnPlayerDragPlace` → `SendClick(containerId, slot, button, mods, actionType)`; `RenderPreview` draws `InventoryState.cursor`. `SlotGridComponent` learns its `container_id` (`SetContainerId`) and authority (`SetAuthoritative`) so clicks carry the right container. ⚠️ Legacy `OnSlotActivated` optimistic mutation path is still compiled (dual-mode, D9) — deletion is Phase E 5.2/5.4.

### D7 — Keep craft *validation* server-side as today ⚠️ deviated
`CraftRequestHandler` stays the authority for "does this grid craft". It now reads the grid from the **server** (`WorkbenchStateManager::getGridState` — client-supplied slots ignored) and consumes inputs server-side. **Deviation**: the grid is a server-synced **staging area**, not a real item-holding container — `CraftRequestHandler` still deducts the consumed items from the **player inventory** and `GridUpdate` is retained as the craft-feedback channel (see D12). The `kGridFlag` external-drag path and `HandleActivate` staging are gone (zero refs in `CraftingGrid.cpp`).

### D8 — Phase A is an atomic server+client cutover (verified) ✅ implemented
`InventoryAction` semantics are breaking and the click handler replaces the legacy handler on the single `player.inventory.actions` topic → there is **no safe partial state**. Old client + new server = every op silently no-ops; new client + old server = clicks ignored. Server + client landed in ONE commit (`f8d1e4a`, squashed on main).

### D9 — DragManager stays dual-mode through the container phases ✅ implemented
Chest/machine/workbench windows all depend on DragManager's optimistic mutation for their drags today. Cutting mutation in Phase A would break them before B/C/D convert. So Phase A adds a **click path for the player context** while **retaining the mutation path for unconverted containers**. All three containers converted (B/C/D); mutation path deletion is Phase E 5.2/5.4.

### D10 — Machine session writes the SAME ECS `InventoryContainer` MachineSystem ticks ✅ implemented
Machine slot state is authoritative on simcore as an ECS `InventoryContainer` component. The machine session reads/writes that component **in place** — `ContainerSession::slotsRef()` re-resolves `reg.try_get<InventoryContainer>()` on EVERY access (EnTT packed storage can relocate components across ticks; block-clear removes them; cached pointers dangle). Layout compatibility enforced by `static_assert` (`InventorySlot` ≡ `PersistSlot`). Also implemented: `ItemFlowHandler` converted through the session (`forEachOpenAt` — no more synthetic `SetMachineSlotReq` with the `player_slot=0`/255-meta wipe bug), and `MachineSystem` gained a `PublishFullInventory` hook (MachineSystem.cpp:439) on recipe consume/output so the machine window goes live. ⚠️ Server-side `MachineSlotHandler` + `player.machine.slot` topic + gateway route still wired (client never sends it) — deletion is Phase E 5.3.

### D11 — Chest load must gate clicks ✅ implemented
There was **no client→server chest.open request** (load was async after right-click). Phase B added `chest.open` (`kChestOpenReq=19` → `player.chest.open`, template: `kWorkbenchOpenReq→sim.workbench.load`) and gates clicks until the session registers + async `loadSlots` completes — clicks with `container_id=1` before session load are dropped with a warn (authoritative no-op) — preserving the `dataLoaded_` guard semantics, so the first per-action save cannot race the in-flight load and wipe fresh state (item dup/loss).

### D12 — Workbench grid = world-bound server-synced staging (Phase D decision) ✅ decided
The original plan (D4/D7) modeled the workbench grid as a real item-holding container whose cells physically own items, with `CraftRequestHandler` consuming from the grid and `GridUpdate` dropped. Phase D implemented a **different model**:

- The grid is a **staging area keyed by block position** (`WorkbenchStateManager`), shared across players opening the same workbench (GTNH-style; deviates from vanilla return-grid-on-close). Items physically live in the **player inventory**.
- Grid clicks (container_id=1 session, kind=Workbench) mutate the staging cells via the same rule table and persist via `setGridState`.
- Craft reads the server grid, consumes recipe inputs **from the player inventory** (deduction loop retained), grants the result via `giveItem`, and publishes the remaining grid via `sim.workbench.state` → `kGridUpdate=43` (retained) plus the container snapshot.

Rationale: moving items physically into grid cells would have required reworking the recipe consumption contract (machine recipes consume from containers, workbench from inventory) and reopening the persistence/session semantics — a larger change with no user-visible benefit at this stage. Dupe risk is still closed because the grid read is server-authoritative (the client can no longer inject slots).
- **Consequence to track**: `kGridUpdate` remains a live protocol message (not dropped); per-player sessions are separate copies — concurrent players editing the same workbench see last-write-wins per cell without live cross-player broadcast. Documented in spec deltas.

## Risks / Trade-offs

- **Big change surface** → phased: Step 0 (build bootstrap) → A (atomic) → B → C → D → E. ✅ A–D landed; only E (cleanup) remains.
- **MachineSlotHandler + ItemFlowHandler coupling** → both converted through the container model in Phase C; energy/recipe writes stay in MachineSystem on the same ECS container. ⚠️ Server-side `MachineSlotHandler` still wired (dead client-side) → Phase E 5.3.
- **Chest save-on-close removal** → replaced by live per-action persist (Phase B); close now only *deregisters*. ✅ Old `SendChestSaveReq`/`player.chest.save` path deleted. `kChestSaveReq=18` re-purposed as `kMachineOpenReq`.
- **Workbench grid shared-vs-private** → **resolved in D12**: world-bound shared staging + player-inventory consumption; `GridUpdate` retained as craft feedback. ✅
- **Two snapshot sources for MachineWindow** (BlockEntityUpdate energy/progress, InventoryUpdate slots) → explicit field ownership; MachineWindow reads slots only from InventoryUpdate. ✅
- **Legacy `simulation_core/InventoryActionHandler.*`** → dead but defines `ItemStack`/`InventorySlot` structs imported by `ElectricDrillHandler.cpp`/`ItemEnergyStorage.h`/`test_main.cpp`; **split the structs into a shared header before deleting** the handler → Phase E 5.1.
- **Build bootstrap** → Step 0 completed (conan+cmake+ninja+go stubs); worktree build green. ✅
- **OH3 open/load race** → session registered before async load; pre-load clicks dropped. ✅

## Migration Plan

0. **Step 0 — Build bootstrap** ✅ done: conan install + cmake configure + meta_db `--go` + ninja + ctest baseline 11/11.
1. **Slice 1 — Phase A, atomic (server + client, player window)** ✅ done: D2 click descriptor + D3 snapshot + schema-comment fix; PlayerInventoryStore cursor; pure rule table + unit tests; click handler on the topic; client click-translator + cursor rendering + snapshot-driven player window; DragManager dual-mode. Ship: player inventory fully server-authoritative, containers untouched.
2. **Slice 2 — Phase B (chest)** ✅ done: chest.open/close + session registration; load-gating before clicks; chest slots through the rule table (container_id=1); ChestWindow snapshot-driven + null-`inv_` fix; decoupled bundled player persistence; `SendChestSaveReq`/`player.chest.save` removed.
3. **Slice 3 — Phase C (machine)** ✅ done: machine session on the SAME ECS InventoryContainer; ItemFlowHandler converted (arg bug fixed); MachineSystem→InventoryUpdate publish hook; MachineWindow switched to InventoryUpdate slots; client machine-drag context + per-slot `SetMachineSlotReq` movement retired (server handler → Phase E 5.3).
4. **Slice 4 — Phase D (workbench)** ✅ done (D12 deviation): shared-vs-private decided (world-bound staging); grid as container session (9 cells, WorkbenchStateManager + live publish); CraftingGrid snapshot-driven, `kGridFlag`/`HandleActivate` gone; CraftRequestHandler reads server grid (player-inventory deduction retained by design); `GridUpdate` retained as craft feedback.
5. **Slice 5 — Phase E (cleanup)** ⏳ pending: split ItemStack/InventorySlot structs out of the legacy header first, then delete the legacy handler; delete dead RenderSlotGrid + never-wired callbacks; fix the real RMB-distribute hover defect; delete server-side MachineSlotHandler + gateway SetMachineSlotReq route; delete `kGridSlotBase`/`kMachineSlotBase`/`kMachineOutputBase` numbering; rework client tests + add server lifecycle tests; full ctest + run.sh manual pass + git push.

## Open Questions

- Container id assignment: **resolved — per-player session scope** (see D4). ✅
- Should `InventoryUpdate` carry the container, or a separate `ContainerUpdate` message? **Resolved — one message** (D3); no BlockEntityUpdate overlap issues in practice. ✅
- **Workbench shared-vs-private grid**: **resolved in D12** — world-bound shared staging; craft consumes player inventory; `GridUpdate` retained. ✅
- **Request-id correlation** (G13): **resolved by the snapshot-only model** — open/close acks ride the `player.inventory.update` → `kInventoryUpdate` relay; windows filter by `container_id`, no ack request-id needed. ✅
- **Chest entity_type / schema**: **resolved — reuse the existing `MachineState` blob with `kChestEntityType=3`** (`ChestStateManager::EncodeChestBlob`/`DecodeChestBlob`); single ESS path. ✅
- **MachineSystem publish cadence** (G4): **resolved — publish after every recipe consume/output** (`PublishFullInventory`, MachineSystem.cpp:439); simcore-side publish is cheap and the client window stays live. ✅
