# Design: Add Pipe Fluid Overlay

## Context
The wrench overlay (`WrenchOverlay` + `ImGuiOverlay::DrawOverlay`) already projects a 3D block into screen space (8 corners, 12 edges, 6 faces via `wrench_overlay::kFaceCorners`) and draws a per-face connectable grid. The wrench path is contextual: it only shows while holding `ITEM_WRENCH` and targeting a block adjacent to a pipe/cable. There is no general, persistent overlay for inspecting fluid pipe connections.

`PipeMeshBuilder::detectConnections(x,y,z,type,getBlock,getMeta)` returns the same `FaceMask` the chunk mesh builder uses to decide which pipe faces get geometry — so reusing it guarantees the overlay matches the real rendered connections.

## Goals / Non-Goals
- Goals: GTNH-style fluid pipe connection visibility, toggleable, client-only, zero protocol impact.
- Non-Goals: network-wide highlighting, live server fluid-content/flow display, cable/heat pipe overlays (future phases).

## Decisions
- **Toggle state** lives on `ActionHandler` (`pipeFluidOverlayOn_`, default false) with a `PipeFluidOverlayOn()` getter; `GameClient` reads it each frame via `uiMgr_.GetActions()`. This keeps the toggle with the action that flips it, mirroring how other toggles live in the UI/action layer.
- **Keybind**: `GLFW_KEY_L` → `toggle_pipe_fluid_overlay`. `L` is unused by existing default binds.
- **Targeting**: when toggled on AND the highlight ray targets a fluid pipe block (`BlockRenderRegistry::blockIdToPipeType ∈ {FLUID_PIPE, DENSE_FLUID_PIPE}`), compute `detectConnections` against `World` and store the resulting `FaceMask` in `FrameExt.pipeFluidConnectable[6]` + `pipeFluidIsDense`.
- **Rendering**: in `DrawOverlay`, when `showPipeFluidOverlay`, for each connected face `f` draw an `AddQuadFilled` (fluid blue, ~85 alpha) + `AddQuad` outline (lighter) using `wrench_overlay::kFaceCorners[f]` screen corners. Dense fluid = brighter blue.
- **No protocol change**: purely client side; reads `World` block/meta + `detectConnections`.

## Risks / Trade-offs
- Targeted-block-only (not network-wide) in Phase 1 — cheaper, consistent with the existing single-block wrench overlay, and avoids per-frame scan of all loaded chunks. Network-wide view is deferred (see Out of Scope).
- `detectConnections` allocates two `std::function` callbacks per frame; negligible (one call/frame when toggled + targeting a fluid pipe).

## Migration Plan
Backward compatible: client-only, no data/schema/protocol. Rollback = revert the edits.

## Out of Scope (future phases)
- Network-wide fluid pipe highlighting (needs iterating loaded chunks / spatial query).
- Live fluid content / flow rate display from the server (would need a new protocol message).
- Cable/heat-pipe connection overlays (same mechanism, different block filter).
