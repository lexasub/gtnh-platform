# Design: Add Pipe Fluid Overlay

## Context
The wrench overlay (`WrenchOverlay` + the static `RenderBridge::ImGuiOverlay` callback in `Render/RenderBridge.cpp`) already projects a 3D block into screen space (8 corners, 12 edges, 6 faces via `wrench_overlay::kFaceCorners`) and draws a per-face connectable grid. The wrench path is contextual: it only shows while holding `ITEM_WRENCH` and targeting a block adjacent to a pipe/cable. There is no general, persistent overlay for inspecting fluid pipe connections.

`PipeMeshBuilder::detectConnections(x,y,z,type,getBlock,getMeta)` returns the same `FaceMask` the chunk mesh builder uses to decide which pipe faces get geometry — so reusing it guarantees the overlay matches the real rendered connections.

## Goals / Non-Goals
- Goals: GTNH-style fluid pipe connection visibility, toggleable debug overlay,
  and enough server-authoritative state to diagnose source/pipe/sink flow.
- Non-Goals: network-wide highlighting, client-side fluid simulation, direct
  client-to-PipeNetwork requests, cable/heat pipe overlays (future phases).

## Decisions
- **Toggle state** lives on `ActionHandler` (`pipeFluidOverlayOn_`, default false) with a `PipeFluidOverlayOn()` getter; `GameClient` reads it each frame via `uiMgr_.GetActions()`. This keeps the toggle with the action that flips it, mirroring how other toggles live in the UI/action layer.
- **Keybind**: `GLFW_KEY_L` → `toggle_pipe_fluid_overlay`. `L` is unused by existing default binds.
- **Targeting**: when toggled on AND the highlight ray targets a fluid pipe block (`BlockRenderRegistry::blockIdToPipeType ∈ {FLUID_PIPE, DENSE_FLUID_PIPE}`), compute `detectConnections` against `World` and store the resulting `FaceMask` in `FrameExt.pipeFluidConnectable[6]` + `pipeFluidIsDense`. Note: this intentionally differs from the wrench path, which uses an inline neighbor+meta-bit loop (`GameClient.cpp:485-518`); the overlay uses `PipeMeshBuilder::detectConnections` (per spec) so it matches mesh construction, and reuses `wrench_overlay::kFaceCorners` only for screen-space projection.
- **Rendering**: in the `RenderBridge::ImGuiOverlay` callback (`Render/RenderBridge.cpp`), when `showPipeFluidOverlay`, for each connected face `f` draw an `AddQuadFilled` (fluid blue, ~85 alpha) + `AddQuad` outline (lighter) using `wrench_overlay::kFaceCorners[f]` screen corners. Dense fluid = brighter blue. (The wrench path draws with `AddLine` only; the quads are new drawing code.)
- **Authoritative debug state**: topology uses local `World` block/meta and
  `detectConnections`; amounts use the existing validated client-facing
  `ResourceBufferStateStore` (`Network/ResourceBufferStateStore.h`, fed by
  `kResourceBufferState = 47` / topic `sim.resource.buffer.state` from
  SimulationCore's `ResourceBufferStatePublisher` — commit 36cc06c). Do not add
  an internal PipeNetwork subscription to the client.
- **Display semantics**: show `unknown/stale` when no current server snapshot
  exists; never render a missing value as `0`. Staleness is structural: the
  store gates updates by epoch/sequence and `FindAt()` returns `nullptr` when
  no current snapshot exists — the overlay renders that as `unknown/stale`.
  The resource name is resolved at display time via `ItemRegistry::GetName`;
  the store keeps only the canonical packed ID.
- **No client simulation**: the overlay is read-only and never sends a consume,
  drain, or registration request.

## Risks / Trade-offs
- Targeted-block-only (not network-wide) in Phase 1 — cheaper, consistent with the existing single-block wrench overlay, and avoids per-frame scan of all loaded chunks. Network-wide view is deferred (see Out of Scope).
- `detectConnections` allocates two `std::function` callbacks per frame; negligible (one call/frame when toggled + targeting a fluid pipe).

## Migration Plan
Backward compatible: client-only, no data/schema/protocol. Rollback = revert the edits.

## Out of Scope (future phases)
- Network-wide fluid pipe highlighting (needs iterating loaded chunks / spatial query).
- Live flow-rate / network-wide fluid topology from the server (buffer snapshot text via the existing `kResourceBufferState` channel IS in scope; per-tick flow rates would need a new protocol message).
- Cable/heat-pipe connection overlays (same mechanism, different block filter).
