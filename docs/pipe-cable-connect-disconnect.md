# Pipe / Cable CONNECT & DISCONNECT — Bug Report + Handoff

**Status:** OPEN · Priority P1 · Bead: `GTNH-u0v` · Updated: 2026-08-11
**Project:** `gtnh-platform` (C++ voxel engine: client `game_client` + server `simulation_core` + server `pipe_network`; Go sidecars)
**TL;DR:** Wrenching a pipe/cable face should disconnect *that* face on both adjacent blocks. Pipes currently do **not** disconnect. A prior fix (`d32d8d9`) only restored **local-client VISIBILITY** of the toggle — it did **not** fix the disconnect mechanic. This document separates observed facts from unverified hypotheses and is intended as clean input for the next step.

---

## 1. OBSERVED BEHAVIOR (facts — not conclusions)

> Label key: `[USER]` = reported by user, not independently code-verified by us. `[CODE]` = verified in source / by build.

- **[USER] O1** — Wrenching a pipe/cable connection face does **not** disconnect the pipe. Pipes remain connected and flow continues. Reported 2026-08-11.
- **[USER] O2** — (Prior session) The wrench toggle on pipe/cable was **invisible to the acting local player** (their own mesh did not rebuild); other clients saw it.
- **[CODE] O3** — `WrenchActionHandler::publishBlockChanged` now sets `source_player_id = 0` (changed from the acting player in commit `d32d8d9`). Intent: make Gateway relay the `world.blocks.changed` event to **all** clients incl. the actor.
- **[CODE] O4** — `gateway.cpp` (~line 2437) drops `world.blocks.changed` for a subscriber when `p.player_id != 0` (optimistic BlockAck assumption); `player_id == 0` early-returns and relays to all. So after `d32d8d9` the acting client receives the event.
- **[CODE] O5** — `simcored_test` passes **454/0** after `d32d8d9` (no simulation regression).
- **[CODE] O6** — Committed extraction: `WrenchMeta.h` (`computePipeToggle`, meta `0 → 0x3F`), `MeshHash.h`, `WrenchOverlay.{cpp,h}`, `test_wrench_meta.cpp`. **None are referenced in any `CMakeLists.txt`** (not built on origin).
- **[TRACKER] O7** — Beads: `GTNH-dii` (local visibility) **CLOSED**; `GTNH-u0v` (disconnect still broken) **OPEN, P1**.

---

## 2. EXPECTED BEHAVIOR — Functional

- **F1** — Wrenching a pipe/cable face severs **that face** on **both** adjacent blocks (target bit toggled + neighbor opposite bit toggled).
- **F2** — After disconnect, fluid/energy/item flow across that face **stops**; the two sides become independent networks.
- **F3** — The disconnected state **persists**: stored in block meta, flows `setBlockCAS → BlockChangedEvent(meta) → client mesh + PipeNetwork`.
- **F4** — All clients incl. the actor see the disconnected mesh (believed fixed by `d32d8d9`; re-validate once disconnect works).
- **F5** — Backward compat: a freshly placed pipe (meta `0`) connects all 6 faces geometrically.

---

## 3. NEGATIVE / Edge cases

- **N1** — Disconnect must **not** delete/remove the pipe block itself.
- **N2** — Must **not** disconnect unintended faces (only the targeted one, on both blocks).
- **N3** — Must **not** crash or corrupt chunk/meta state.
- **N4** — Must **not** regress `simcored_test` (keep 454/0) or break the PipeNetwork solver.
- **N5** — Must **not** re-introduce the local-visibility bug (actor mesh must update).
- **N6** — Must **not** affect machine→pipe edges (those are explicit ECS `connected_nodes`; the connection mask applies only to pipe↔pipe spatial-adjacency edges).

---

## 4. COMPATIBILITY (interactions + verified vs unverified)

| System | Interaction | State |
|--------|-----------|-------|
| PipeNetwork | Disconnect must update the network graph + trigger re-solve. PipeNetwork caches unchanged networks ~5s → may need cache invalidation. | **Unverified** (mask-aware edge logic may be unimplemented) |
| ChunkStore | Connection state lives in block meta; must go through `setBlockCAS`/`SetBlockMeta`. | **Unverified** — critical: does meta-only change emit `BlockChangedEvent`? |
| Gateway relay | `source_player_id = 0` ensures all clients receive the update. | **Verified** (O3/O4) |
| MeshManager / PipeMeshBuilder / CableMeshBuilder | `detectConnections` must be mask-aware (read meta; meta `0 → 0x3F`). | **Unverified** |
| Multiblock boundaries | Disconnect must not cross chunk/multiblock incorrectly. | Unverified |
| simcored_test | No regression. | **Verified** 454/0 (O5) |

---

## 5. HYPOTHESES (NOT verified — require investigation; do **not** treat as facts)

- **H1 — Server toggle does not actually write the connection state.** The wrench disconnect action may only flip a visual/overlay flag, not block meta; or the meta-toggle branch in `WrenchActionHandler.cpp` (ex-lines 67–89, the `if (r.error == "no_machine_at_position" && blockRepository_)` pipe branch) was never correctly implemented or was bypassed; or `setBlockCAS` is called but does **not** emit a `BlockChangedEvent` carrying the new meta, so neither the client mesh nor PipeNetwork sees the change.
  - *Verify:* grep `setBlockCAS` implementation + the `BlockChangedEvent` publish site (likely `ChunkEventHandler` / a chunk-store repository). Confirm a meta-only change emits the event with `meta()`.
- **H2 — PipeNetwork/CableGraph edge logic is not mask-aware.** Even if meta updates, the solver still treats the face as connected and flow continues.
  - *Verify:* inspect `PipeNetworkManager::addEdge` and `CableGraph::rebuildGraph` for the meta-bit skip check (both nodes' bits set for the shared face).
- **H3 — Relay fix addressed only the visibility symptom.** Disconnect and visibility were always two independent bugs (or the relay fix never reached the binary the user tested).
  - *Verify:* confirm the running binary includes `d32d8d9`; reproduce disconnect in-client.
- **H4 — Disconnect requires a distinct, unwired action path.** Wrench "disconnect" vs "cycle/connect" may be separate `ToolActionType`s and the disconnect variant isn't handled.
  - *Verify:* trace `WrenchActionHandler::handle()` / `wrenchHandler_->cycleFace` for pipe/cable handling.

---

## 6. RULES & CONSTRAINTS

### Project-wide (apply to all work in this repo)
- **PW1 — Working code wins.** If docs/spec and implementation conflict, implementation is truth; a spec describing a NEW feature may be unimplemented. **Verify in code before trusting any recon below.**
- **PW2 — Build/test discipline.** Linux-only. **Never** rebuild `cmake-build-debug/` from scratch; **never** delete `cmake-build-*` (holds Conan toolchain). Incremental: `cd cmake-build-debug && ninja -j5`. Tests: `ctest --output-on-failure -j$(nproc)`.
- **PW3 — Task tracking via `bd` (beads); no markdown TODO lists.**
- **PW4 — Shared zones** (`src/protocol/`, `data/registry/`, `data/recipes/`, `CMakeLists.txt`, `conanfile.txt`): `git pull --rebase` before touching.
- **PW5 — Game modes.** Default `GameMode` stays **CREATIVE** (don't flip to SPECTATOR). Break/place per-mode gating lives in `GameClient.cpp:332` (leave as-is); full interaction semantics belong to a SEPARATE openspec change `add-interaction-mode-gating`, **not** this work. Keep wrench disconnect out of `add-game-modes`.
- **PW6 — Repro env (nvidia VM).** To run the client GUI, prefix: `__VK_LAYER_NV_OPTIMUS=1 __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia`.

### Task-specific (this bug / wrench work)
- **TS1 — Scope separation.** Keep wrench/pipe-toggle changes **separate** from the unrelated, **half-done MachineRole refactor** (mixed in dirty `simcored/CMakeLists.txt`, `GameClient.cpp`, `simcored_test/CMakeLists.txt`). **Do NOT commit MachineRole.**
- **TS2 — Wire extracted units.** `WrenchMeta.h`, `MeshHash.h`, `WrenchOverlay.*`, `test_wrench_meta.cpp` are **not** in any `CMakeLists`. Wire them in a **dedicated scoped commit**, free of MachineRole.
- **TS3 — Don't touch unrelated dirty files:** `AGENTS.md`, `data/registry/machines.yaml`, `openspec/*`, `.claude/settings.json`, `.beads/interactions.jsonl`.
- **TS4 — Don't modify** `src/services/game_client/Render/RenderBridge.cpp` (GT-style wrench overlay already implemented there).
- **TS5 — Don't strip** the temp debug field `heldItemId` in `RenderAPI.h` / `GameClient.cpp` (user instruction).
- **TS6 — Beads:** `GTNH-dii` (visibility) is fixed & CLOSED — don't reopen or conflate. `GTNH-u0v` (disconnect) is the active bug — keep OPEN, P1.
- **TS7 — Match client style:** GLM, FlatBuffers, EnTT, spdlog, ImGui.

### Enforcement (hard rules)
- **E1 — No type-error suppression** (`as any` / `@ts-ignore` / casts hiding errors / warnings that break the build).
- **E2 — Never commit without explicit request** — but when committing, scope strictly (wrench only; no MachineRole; no unrelated dirty files).
- **E3 — Work is not complete until pushed to origin** (`git push`). For beads, persist via `issues.jsonl` commit + push (no dolt remote configured).
- **E4 — Never delete failing tests** to make build/tests pass.
- **E5 — Read large files in small windows** (`read` with `offset`+`limit`, or targeted `grep`). Never read a whole huge file in one message (this burned two prior agents).

### Out-of-scope rules from other projects (listed only to confirm non-omission — NOT applicable here)
- `iodre_modules`: nested `umf/` structure, duplicated `dispatch.zig` crutch, `umf-modules/src/modules/` planned split, `iplatform` core freeze, codegen crash — **does not apply to gtnh-platform**.

---

## 7. OUTPUT / NEXT-STEP INPUT (acceptance + plan)

**This document is pure input: no conclusions are drawn; hypotheses in §5 are flagged for verification, not asserted.**

The next session must deliver:
1. Locate the authoritative pipe-connection state + the wrench disconnect **server** path (`WrenchActionHandler` → `wrenchHandler_->cycleFace` / `setBlockCAS`).
2. **Confirm** whether the toggle actually mutates block meta via `setBlockCAS`, and whether `setBlockCAS` emits `BlockChangedEvent` **with meta** (H1).
3. Make `PipeNetworkManager::addEdge` + `CableGraph::rebuildGraph` **mask-aware** (H2).
4. Add a focused test; validate via `simcored_test` + a client repro (PW6).

**Acceptance:** wrenching a pipe face disconnects it for all clients and stops flow; no regression (`simcored_test` 454/0); backward compat (meta `0` → all 6 faces) preserved.

---

## Appendix A — Meta convention (authoritative, use everywhere)
- Block meta byte, **bits 0–5 = connection mask**. Bit `i` means face `i` is connected: `bit0=+X, bit1=-X, bit2=+Y, bit3=-Y, bit4=+Z, bit5=-Z`. bits 6–7 reserved.
- **LEGACY/default:** `meta == 0` MUST be treated as "all 6 faces connected" (`0x3F`) everywhere you read it. Only after a wrench toggle does meta become non-zero.
- Face index → world offset: `DX[6]={1,-1,0,0,0,0}`, `DY[6]={0,0,1,-1,0,0}`, `DZ[6]={0,0,0,0,1,-1}`. Opposite face = `face ^ 1` (0↔1, 2↔3, 4↔5).
- `FaceMask` constants (`PipeMeshBuilder.h`): `FACE_DOWN=1<<0, FACE_UP=1<<1, FACE_NORTH=1<<2, FACE_SOUTH=1<<3, FACE_WEST=1<<4, FACE_EAST=1<<5`.
- Meta bit `i` → FaceMask: `i0=+X→FACE_EAST, i1=-X→FACE_WEST, i2=+Y→FACE_UP, i3=-Y→FACE_DOWN, i4=+Z→FACE_SOUTH, i5=-Z→FACE_NORTH`.
- Helper:
  ```cpp
  constexpr FaceMask META_BIT_TO_FACEMASK[6] = { FACE_EAST, FACE_WEST, FACE_UP, FACE_DOWN, FACE_SOUTH, FACE_NORTH };
  inline FaceMask metaToFaceMask(uint8_t m){ FaceMask r=0; for(int i=0;i<6;++i) if(m&(1<<i)) r|=META_BIT_TO_FACEMASK[i]; return r; }
  ```
- Item IDs: pipes `0xF800..0xF803`; cables `0xF400..0xF405`; wrench `0xF005 = ITEM_WRENCH` (`src/data/registry/ToolIds.h`). Detect with `ItemId::isPipe(n)` / `ItemId::isCable(n)` (already exist).

## Appendix B — Implementation recon (from prior brief; **verify before use** — PW1)
- **`IBlockRepository`** — `src/services/simulation_core/Storage/IBlockRepository.h`: `setBlockCAS(x,y,z,expected_id,new_id,meta,cb)`, `getBlock(x,y,z,cb)` → `const BlockData&{block_id,meta,mb_id}`. `CASResult{status,block_id,meta}`.
- **`WrenchActionHandler`** — `src/services/simulation_core/Actions/handTool/WrenchActionHandler.cpp` (+`.h`). `handle()` parses `Protocol::ToolAction`, returns early unless `action()==ToolActionType_WRENCH_CYCLE`, 200ms cooldown, then `r = wrenchHandler_->cycleFace(player_id,x,y,z,face)`. On success publishes `ToolActionResp` to `"player.tool.action.response"`. The pipe branch (ex-lines 67–89) is the toggle site to implement/replace.
- **`PipeMeshBuilder.h`** — defines `FaceMask` + `FACE_*` + `PipeType`. `detectConnections(x,y,z,type,getBlock)` — add optional `getMeta` returning `uint8_t` (default `nullptr`).
- **`BlockChangedEvent`** — has `meta()` accessor (`ChunkEventHandler.cpp:38`). `PipeNetworkService::handleBlockChanged` (`pipe_network/PipeNetworkService.cpp` ~265) already does `GetRoot<BlockChangedEvent>` + checks pos — read `event->meta()` there.
- **`ChunkMeshBuilder.cpp`** — neighbor cache `cache` has `GetBlock(bx,by,bz)` (lines 86 & 99). Add `cache.GetMeta(bx,by,bz)` mirroring it.
- **`CableMeshBuilder.cpp`** — `detectConnections` at line 157; same change as PipeMeshBuilder.
- **`GameClient.cpp`** — `highlightedBlockId = world_.GetBlockAt(...)` (404); `interaction_.TargetFace(camera_)` (329); overlay gate ~419–435; `frd.ext.wrenchConnectable[i]` set at 432. Needs `world_.GetMetaAt(...)` for highlighted block + neighbor.
- **`World`** — has `GetBlockAt(BlockPos)`; add `GetMetaAt(BlockPos) const` (match `GetBlockAt`'s chunk lookup + `meta_data()` accessor + idx formula; return 0 if null).

## Appendix C — Distilled implementation plan (from prior 5-step brief)
1. **Foundation:** shared `metaToFaceMask`/`DX/DY/DZ` header (`PipeMeta.h`); add `World::GetMetaAt`; ensure ChunkMeshBuilder cache has `GetMeta`.
2. **Client mesh mask-aware:** `detectConnections` returns `geomMask & metaToFaceMask(mv)` when `mv!=0` (else geomMask); apply to Pipe + Cable builders; pass `getMeta` lambda at callsites (86/99).
3. **Client overlay mask-aware:** gate `wrenchConnectable[i]` on both blocks' meta bits (with `meta==0 → 0x3F`). Don't touch `RenderBridge.cpp`.
4. **Server toggle:** replace pipe branch with direct meta toggle (`cur=(meta==0)?0x3F:meta; newMeta = cur ^ (1<<face)`), toggle neighbor opposite bit via `setBlockCAS`. **Critical:** confirm `setBlockCAS` emits `BlockChangedEvent(meta)`; if not, emit one.
5. **Network mask-aware:** pass meta into `addNode`/`addCableNode`; in `addEdge`/`rebuildGraph`, skip pipe↔pipe edge unless both bits set (machine→pipe edges exempt).
