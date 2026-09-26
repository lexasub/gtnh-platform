# Audit: dead code candidates in game/ui and game/client (gp-gkv)

**Date**: 2026-09-26
**Scope**: `src/game/ui/client` and `src/game/client`.
**Method**: 316 function symbols extracted from `.h`/`.cpp` in both trees (test dirs excluded from
the definition set), then a whole-tree reference count. A symbol is reported dead **only** when
`grep -rnw` over `src tools test doc openspec .beads` returns hits that are only the declaration
and its own definition.

**Read this before acting.** `.claude/worktrees/` holds ~10 parallel agent checkouts of this same
repo. A naive `grep -rnw SYM .` returns 11-40 hits for symbols that are genuinely dead, because
every worktree carries a copy of the same header. Every command below therefore excludes
`--exclude-dir=worktrees`. This is the same trap that made `RenderSlotGrid` look live/dead in
both directions earlier today.

## The exact command used

```bash
cd /home/su/src/local/gtnh-platform
grep -rnw SYMBOL \
  --exclude-dir=cmake-build-debug --exclude-dir=cmake-build-release \
  --exclude-dir=.git --exclude-dir=graphify-out --exclude-dir=worktrees \
  --exclude-dir=venv --exclude-dir=bgfx \
  src tools test doc openspec .beads
```

`--exclude-dir=bgfx` matters: it contains a vendored Dear ImGui with its own
`IsFirstFrame` (`bgfx/3rdparty/dear-imgui/imgui_internal.h:1857`) and `NoClip` flags.

## Ranked findings — CONFIRMED DEAD (1 hit = the declaration itself)

These are the only symbols I would act on. Each has exactly **one** grep hit in the entire tree:
the definition. No caller anywhere, including tests, `tools/`, docs, or string-built references.

### 1. `InputBinder::ActiveContextCount` — src/game/ui/client/core/InputBinder.h:52

```cpp
size_t ActiveContextCount() const { return active_.size(); }
```
1 hit. Zero callers. **Safe to delete** (inline, header-only, no ABI concern).

### 2. `SlotGridComponent::GetSelectedSlot` — src/game/ui/client/components/SlotGrid.h:49

```cpp
int GetSelectedSlot() const { return selectedSlot_; }
```
1 hit. Note the *setter* `SetSelectedSlot` **is** live (`PlayerInventoryGrid.h:42` calls it) —
so the field is written, never read back. Classic half-wired accessor pair.

### 3. `SlotGridComponent::GetHoveredSlot` — src/game/ui/client/components/SlotGrid.h:88

```cpp
int GetHoveredSlot() const { return hoveredSlot_; }
```
1 hit. **Careful — do not delete blindly.** The openspec change
`openspec/changes/refactor-server-authoritative-inventory-verification/tasks.md:8` describes an
in-flight fix that intends to *call* this:

> "fix the **real** RMB-distribute hover defect: `SlotGridComponent` writes `inv_->dragHoverSlot`
> (SlotGrid.cpp:224) but `OnRightDragDistribute` reads `dm_->GetHoverSlot()` — wire hover→`UpdateHover`"

Note that `dm_->GetHoverSlot()` is `DragManager::GetHoverSlot`, a **different** class from
`SlotGridComponent::GetHoveredSlot`. So the planned fix does not make this accessor live. It is
dead, but the neighbouring hover bug is a real open defect — see "Related finding" below.

### 4. `InputManager::IsFirstFrame` — src/game/ui/client/InputManager.h:23

```cpp
bool IsFirstFrame() const { return firstFrame_; }
```
1 hit in our tree. The companion `ClearFirstFrame()` **is** called
(`src/game/client/GameClient.cpp:460`), so `firstFrame_` is set-and-cleared but never read.
**Safe to delete** — the field and its only remaining operation can go together.

### 5. `BlockAttachedWindow::SetAnchorPos` — src/game/ui/client/BlockAttachedWindow.h:21

```cpp
void SetAnchorPos(BlockPos pos) { pos_ = pos; }
```
1 hit. Check whether `pos_` is set another way before deleting; if the whole
`BlockAttachedWindow` anchor mechanism is unwired this is the symptom.

### 6. `ISidePanel::GetTargetBlock` — src/game/ui/client/panels/ISidePanel.h:40

```cpp
virtual BlockPos GetTargetBlock() const { return BlockPos{}; }
```
1 hit — and it is a **virtual with a default that returns an empty BlockPos**. Zero overrides
anywhere. A virtual that nothing calls and nothing overrides.

## Ranked findings — DEAD BY INFERENCE, verify before deleting

These have exactly 2 grep hits: declaration + out-of-line definition, and no call site. The risk is
that a caller is constructed dynamically (function pointer, reflection, macro). I checked for that;
none found, but I rate these "unclear" per the conservative rule.

### 7. `InputBinder` context stack — 4 symbols, all in one cluster

| Symbol | file:line | hits |
|---|---|---|
| `PushContext` | `core/InputBinder.h:48` / `.cpp:110` | 2 |
| `PopContext` | `core/InputBinder.h:49` / `.cpp:121` | 2 |
| `RemoveContext` | `core/InputBinder.h:50` / `.cpp:126` | 2 |
| `IsContextActive` | `core/InputBinder.h:51` / `.cpp:131` | 2 |
| `UnbindAll` | `core/InputBinder.h:28` / `.cpp:101` | 2 |

`grep -rnw PushContext` etc. over the whole tree returns **only** the header declaration and the
`.cpp` definition. The entire context-stack feature of the input binder is unwired: nothing ever
pushes a context, so the context filter that `dispatchBindings` presumably consults is inert.
`IsContextActive` has no test coverage either (the 700-line `GameModeGate_test.cpp` and
`InteractionSystem_test.cpp` do not mention it).

This is the highest-value item in the "unclear" tier because it is **five** functions plus the
`active_` container they maintain — a whole feature that is declared, defined, and never entered.
Before deleting, confirm the intended design: either the UI is supposed to push a context per
open window, or the context system is vestigial. The latter looks likely, since
`InputBinder::Process` at `.cpp:177` is the only real entry point and no window code pushes.

### 8. `ActionRegistry::Unregister` — src/game/ui/client/core/ActionRegistry.cpp:7

```
ActionRegistry.cpp:7:void ActionRegistry::Unregister(const std::string& name) {
ActionRegistry.h:12:void Unregister(const std::string &name);
openspec/changes/archive/2026-09-01-refactor-fluid-port-accounting/tasks.md:116  (prose, different subsystem)
openspec/changes/refactor-fluid-port-accounting/tasks.md:29                    (prose, different subsystem)
```
2 code hits, both declaration/definition. The two openspec hits are English prose about
unregistering *ports*, not this function. Unregistering an input action is never done — actions
registered in `ActionHandler::Init` (`core/ActionHandler.cpp:19-53`) live for the process lifetime.

### 9. `UIManager::CloseAll` — src/game/ui/client/UIManager.cpp:96

```
UIManager.h:121:  void CloseAll();
UIManager.cpp:96:void UIManager::CloseAll() {
src/game/ui/client/panels/ISidePanel.h:12:// OpenExclusive/CloseAll.   (comment)
doc/archive/EPICS/0-basic-mechanics/ui.md:91: ... OpenExclusive, CloseAll, ...  (planning doc)
```
Zero callers. It is a public API on the central UI manager, so a future caller is plausible —
rank below the others.

### 10. `MachineWindow::SetEnergyType` — src/game/ui/client/block/MachineWindow.h:74

```
MachineWindow.h:74:  void SetEnergyType(EnergyType et);
MachineWindow.cpp:187:void MachineWindow::SetEnergyType(EnergyType et) {
```
2 hits. The machine's energy type comes from the registry via `GetEnergyType()`
(`MachineWindow.cpp:183`) and is never overridden at runtime, so the setter has no caller.

### 11. `CreativeMenu::rebuildItemList` — src/game/ui/client/player/CreativeMenu.h:37

```
CreativeMenu.h:37:  void rebuildItemList();
CreativeMenu.cpp:13:void CreativeMenu::rebuildItemList() {
```
2 hits. **Unclear**: a "rebuild the list" method with no caller is suspicious, but
`CreativeMenu::Render` (`CreativeMenu.cpp:47`) may call it indirectly, and the menu is the one
place where the item set changes. I could not rule out a call inside a code path my regex missed
because the method is lowercase-private. Verify by hand before deleting.

### 12. `SetMachineActionCallback` — src/game/ui/client/core/DragManager.h:149

```
DragManager.h:149:  void SetMachineActionCallback(MachineActionCallback cb) { machineCb_ = std::move(cb); }
openspec/changes/refactor-server-authoritative-inventory-verification/tasks.md:8,22  (prose)
```
The openspec file already flags this as **"never-wired"** in its own words:

> "and the never-wired `SetMachineActionCallback`/`SetMachineSlotAckCallback` (G15)"

So a prior audit reached the same conclusion independently. `machineCb_` is therefore always empty.
Corroborated, not just inferred.

## CLEARED — live, do not touch

Recorded because these looked dead under a naive same-file-only grep and cost time:

| Symbol | Live call site |
|---|---|
| `BufferUnit` | `MachineWindow.cpp:307` (same file) |
| `DrawArrowProgress` | `MachineWindow.cpp:225` |
| `DrawFlameProgress` | `MachineWindow.cpp:237` |
| `DrawSpinnerProgress` | `MachineWindow.cpp:231` |
| `EnergyBarColor` | `MachineWindow.cpp:266` |
| `HatchTypeName` | `MachineWindow.cpp:453` |
| `SetServiceHealthStore` | `GameClient.cpp:274` |
| `SetPipeContentsStore` | `GameClient.cpp:273` |
| `ClearFirstFrame` | `GameClient.cpp:460` |
| `SetActionRegistry` | `ActionHandler.cpp:28` |
| `SetTextCapture` | `UIManager.cpp:55` |
| `InvalidatePreview` | `ClientCraftingWindow.cpp:67` |
| `SetSlotIndexOffset` | `ChestWindow.cpp:86` |
| `SetSelectedSlot` | `PlayerInventoryGrid.h:42` |
| `IsRecipeInspectOpen` | `ActionHandler.cpp:58,74` |
| `GetSteamItemId` | `test_recipe_mirrors.cpp:328,382,857` |
| `GetAllItemIds` | `ItemIndex.cpp:7`, `CreativeMenu.cpp:17`, + tests |
| `CanFly` | `GameClient.cpp:329` |
| `NoClip` / `InfiniteItems` | `NeiPanel.cpp:21`, `ActionHandler.cpp:171` + tests |
| `IntersectsAABB` | `World.cpp:54` (+3) |
| `DistanceTo` | `Types.h:35` (Frustum, same header) |
| `GameModeName` | `ConsoleWindow.cpp:59,61` |

`ItemColor` (`components/ItemColor.h:7`) is the one borderline case: 1 code hit (the inline
definition) but 2 doc hits that both describe it as a live component — `doc/c4/level4-client-ui-windows.puml:56`
lists it as a C4 Component, and `doc/texture-atlas-format.md:230` says it is "**TO BE REPLACED**
with UV lookup from atlas", i.e. it is on the replacement list but has not been replaced. Rated
**unclear**: the rarity colouring it provides is probably not wired into any slot renderer yet.
That is a real UI gap, not a dead-code candidate.

## Related finding (not a symbol, worth a bead)

`SlotGridComponent::Render` writes the hovered slot to `inv_->dragHoverSlot` (SlotGrid.cpp:224,
per the openspec note) while `DragManager::OnRightDragDistribute` reads
`dm_->GetHoverSlot()` (SlotGrid.cpp:392, per the same note). These are two different
hover channels, so **right-click-drag distribute reads a hover value nobody writes**. I did not
independently re-derive those two line numbers — they come from the openspec task file, so verify
before filing. If correct, this is a live UI bug, not dead code, and it outranks everything in the
"unclear" tier by consequence.

## Summary table

| # | Symbol | file:line | Hits | Confidence |
|---|---|---|---|---|
| 1 | `ActiveContextCount` | `core/InputBinder.h:52` | 1 | **dead** |
| 2 | `GetSelectedSlot` | `components/SlotGrid.h:49` | 1 | **dead** |
| 3 | `GetHoveredSlot` | `components/SlotGrid.h:88` | 1 | **dead** |
| 4 | `IsFirstFrame` | `InputManager.h:23` | 1 | **dead** |
| 5 | `SetAnchorPos` | `BlockAttachedWindow.h:21` | 1 | **dead** |
| 6 | `ISidePanel::GetTargetBlock` | `panels/ISidePanel.h:40` | 1 | **dead** |
| 7 | `PushContext`/`PopContext`/`RemoveContext`/`IsContextActive`/`UnbindAll` | `core/InputBinder.h:48-51,28` | 2 ea | unclear (5 fns) |
| 8 | `ActionRegistry::Unregister` | `core/ActionRegistry.cpp:7` | 2 | unclear |
| 9 | `UIManager::CloseAll` | `UIManager.cpp:96` | 2 | unclear (public API) |
| 10 | `MachineWindow::SetEnergyType` | `block/MachineWindow.h:74` | 2 | unclear |
| 11 | `CreativeMenu::rebuildItemList` | `player/CreativeMenu.h:37` | 2 | unclear |
| 12 | `SetMachineActionCallback` | `core/DragManager.h:149` | 1 | dead (corroborated by openspec) |

Six confirmed dead (items 1-6), five dead by inference needing a hand check (7-11), one
independently corroborated (12). Nothing here was deleted — this is a report.
