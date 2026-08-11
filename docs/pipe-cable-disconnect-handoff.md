# Pipe / Cable Disconnect — short handoff (observed vs hypotheses)

**Status:** OPEN · P1 · Bead `GTNH-u0v` · 2026-08-11 · **reproduced live**
**Label key:** `[USER]` user-observed · `[CODE]` verified in source / git / build
**Binaries:** confirmed fresh by user (not a stale-binary issue)

**TL;DR:** The full disconnect chain is implemented, committed, and in the build — yet the bug reproduces.
So the gap is *runtime*, not missing code. The observed symptom is the client mesh staying connected; whether flow actually continues across the face was NOT verified. This doc separates observed facts from hypotheses and is clean input for the next step.

---

## 1 · OBSERVED (facts)

- **[USER] O1** — Wrenching a pipe/cable face does **not** disconnect (observed live today, 2026-08-11). The observed symptom is on the **client mesh / connection visuals** — it was **not** verified whether fluid/energy/item actually still flows across the face.
- **[CODE] O2** — The entire chain is present, committed, and wired into the build:
  - Client → server: `InteractionSystem.cpp:83` / `GameClient.cpp:345` send `WRENCH_CYCLE`; `gateway.cpp:532` → `player.tool.action`; `ToolActionHandler.cpp:22` → `player.wrench.action`; `WrenchActionHandler.cpp:86–129` pipe branch → `computePipeToggle` (`WrenchMeta.h`) → `setBlockCAS` host+neighbor → `publishBlockChanged` (`source_player_id=0`).
  - Server consumption: `PipeNetworkService.cpp:270–345` (subscribed :136) — cables via `setCableMeta`/`addCableNode`; pipes via `setNodeMeta` + `removeEdgesForNode` + `connectNodeNeighbors`, both using `pipeFacesConnected`/`pipeFaceOpen` (`PipeNetwork.h:26–32`, both bits, `meta==0→0x3F`). `CableGraph::rebuildGraph:56–114` is mask-aware.
  - Client mesh: `ChunkView` decodes meta from wire; `ChunkNeighborCache::GetMeta`; `MeshManager::OnBlockUpdate:90–101` rebuilds chunk **and boundary neighbors**; both `detectConnections` apply `metaToFaceMask` (`PipeMeshBuilder.cpp:189`, `CableMeshBuilder.cpp:157`).
  - Meta-bit order `{+X,−X,+Y,−Y,+Z,−Z}` is consistent across every link (`WrenchMeta.h`, `PipeMeta.h`, `FACE_DX`, `CableGraph` offsets).
- **[CODE] O3** — Key files are tracked and in the build: `WrenchActionHandler.cpp` (`simcored/CMakeLists.txt:108`), `test_wrench_meta.cpp` (:208), `WrenchMeta.h`, `WrenchOverlay.*`. Fix commits present in history: `d32d8d9`, `8ecc56d`.

## 2 · ROOT CAUSE FOUND (Loki live test, 2026-08-11)

**The whole chain works.** Live logs (Loki `192.168.2.109:13100`, `source="tcp-bridge"`) prove it:
- Each wrench click toggles meta on the SAME block: `63 → 59 → 63 → 59` (`0x3B = 0x3F ^ 0x04` = bit2 = **+Y / UP**).
- Client receives every event (`NetClient: BlockUpdate … meta=59/63`, `recv_block_changed`).
- Gateway relays (`relay_block_changed` ×N, `SEND bulk type=4 len=36`).
- Server `WrenchActionHandler` → `computePipeToggle` → `setBlockCAS` → `publishBlockChanged` all verified.

**The bug is face selection, not the chain.**
- `GameClient.cpp` right-click wrench: `face = barFace >= 0 ? kBarFaceToWire[barFace] : interaction_.TargetFace(camera_)`.
- `TargetFace(camera_)` returns the **near** face (the one facing the camera). Clicking a pipe therefore toggles **UP** (+Y, bit2), never the X-face toward the neighbour pipe `(127↔128)`. That's why pipe↔pipe won't detach — the wrong face keeps getting toggled.
- Same for the G-key wrench in `InteractionSystem.cpp` (also used `TargetFace`).
- `pipe→machine` (boiler): same wrong-face issue; the pipe↔machine edge in `connectNodeNeighbors` (`pipeFaceOpen(sourceMeta,f)`) IS gated by the pipe meta, so once the correct face is toggled it should work — but the boiler also must be registered in `machine_nodes_` via `energy.node.update` (unverified for this test).

## 3 · THE FIX (applied in working tree)

Client-only edits, all built successfully (`gameclientd`). **GTNH-compatible wrench interaction** (verified against GT5U source by a code agent + Hermes):
1. **`World/WrenchingSide.h`** (NEW) — pure `determineWrenchingSide(sideHit, u, v)`, exact GTNH `GTUtility.determineWrenchingSide`: 3×3 UV zones on the clicked face, thresholds `0.25` / `>=0.75` (hit coords quantized 1/16), centre→face, edge→neighbour, corner→opposite. `mod = frac()` wrap. This is the GT4/GT5-lineage hit-test GTNH actually uses for clicks (NOT bars/crosses).
2. **`Raycaster::RaycastHit`** (NEW) — returns hit block + sideHit + local UV; **`InteractionSystem::RaycastHitAtMouse`** — ray from the mouse pixel (unProject).
3. **`GameClient.cpp`** — wrench click = `RaycastHitAtMouse → determineWrenchingSide(sideHit, u, v) → ToolAction(face)`. No 2D bar/cross hit-testing.
4. **`RenderBridge.cpp`** — GTNH nine-grid overlay: 3×3 grid on the faced face + amber X-crosses in the zones of CONNECTED sides (`wrenchConnectable`), far face drawn at all 4 corners (matches `BlockOverlayRenderer`). Decorative only — clicks go through UV zones.

GTNH facts (verified): grid overlay = GT5U `BlockOverlayRenderer` (GTNH-era addition, not original GT5); click hit-test = `determineWrenchingSide` UV zones (original GT4/GT5); grid is functional (click toggles) but mapping is UV-zone, not geometry.

## 3 · VERIFIED (chain works) — superseded by §2

The old H1–H4 (mesh not updating / dispatch drop / relay drop / event ordering) are all **ruled out** by the Loki live test: server publishes, gateway relays, client receives. They are kept here only for completeness.

## 4 · NEXT STEP (one session)

1. **Build the fix** (`cd cmake-build-debug && ninja -j5` — client target), run `ctest --output-on-failure -j$(nproc)` (keep simcored 454/0 + pipe_network_test green).
2. **Repro**: with a wrench over a pipe, click the **amber bar** on a connected face → it should disconnect; click the **gray bar** on a disconnected face → reconnect. Clicking the pipe body (no bar) toggles the far face.
3. Verify **pipe→pipe detach** now works and **pipe→boiler** attach works (confirm the boiler is registered as a machine node — `Registered energy node …` in Loki).
4. Update bead `GTNH-u0v` with the result. Do **not** reopen `GTNH-dii` (visibility is fixed).

## 5 · ROOT CAUSE #2 — FIXED (Hermes, 2026-08-11)

The §2 fix (UV-zone click) was itself inverted. Two client bugs, both fixed in the working tree:

1. **`GameClient.cpp` right-click: face-normal → sideHit mapping was flipped on all six cases.**
   `Raycaster::RaycastHit` returns `info.face = -lastStep` = the OUTWARD normal of the face
   the ray ENTERED (faceY==-1 → entered -Y/DOWN). The early code mapped `faceY==-1 → UP`,
   `faceZ==-1 → SOUTH`, … — the exact opposite (its own comment contradicted the older,
   correct `InteractionSystem::TargetFace`). Result: a click on the *visible* face toggled
   the *opposite* face — the whole grid was mirrored through the block, so the natural
   flow (click the face you see, connect the pipe you see) never worked; only random
   clicking "everywhere" hit the right face.
   Fix: `faceNormalToWireSide(faceX,faceY,faceZ)` added to `World/WrenchingSide.h` (same
   mapping as `TargetFace`), used in `GameClient.cpp` right-click.

2. **`RenderBridge.cpp` overlay: `faced = argmax dot(kFaceNormal, camera→block)` picked the
   FAR face** (normal co-directional with the camera vector), so the grid was drawn
   projected from the far side of the block — mirrored visuals. Fix: `argmin` (the near
   face, the one the click ray enters). Overlay and click now share the same face.

3. **G-key** (`InteractionSystem.cpp`) previously sent `TargetFace(camera) ^ 1` (deliberate
   "far face" hack). With the right-click fixed to GTNH semantics, G-key now sends
   `TargetFace(camera)` — same as the click. Far face is reachable via a grid corner.

Verification: `gameclientd` builds; `gameclient_wrench_grid_test` **184/184** (was 172; +12
regression checks for the face→side mapping and the "centre-click toggles the entered face"
guarantee); `ctest -R "wrench|pipe_network|simcored"` 4/4 green.

Next repro: with a wrench over a pipe, click the CENTRE of the visible face → that face
toggles (was: the far one). Click an edge strip → the neighbouring side; a corner → the
far face. Pipe↔pipe detach: look at the pipe whose face meets the neighbour, click the
corner/edge cell of that face.

---

Long-form context: `docs/pipe-cable-connect-disconnect.md` (its Appendix A — meta convention — remains authoritative; its hypothesis section §5 is superseded by this doc).
