# Tasks: Add Pipe Fluid Overlay

## 1. OpenSpec + scaffold
- [x] 1.1 Write `proposal.md`, `tasks.md`, `design.md`, delta `spec.md`
- [x] 1.2 Run `openspec validate add-pipe-fluid-overlay --strict`

## 2. Toggle action + keybind
- [ ] 2.1 Add `DoTogglePipeFluidOverlay` / `PipeFluidOverlayOn()` to `ActionHandler` + register action
- [ ] 2.2 Add default bind `L` → `toggle_pipe_fluid_overlay` in `InputBinder::registerDefaults`

## 3. Frame data + computation
- [ ] 3.1 Add `FrameExt` fields for the overlay in `RenderLib/Common/RenderAPI.h` (`FrameExt` lives there; `RenderBridge.h` only includes it)
- [ ] 3.2 Compute `showPipeFluidOverlay` / `pipeFluidConnectable[6]` / `pipeFluidIsDense` in `GameClient.cpp` via `detectConnections`
- [ ] 3.3 Read debug amount/capacity/resource ID only from `ResourceBufferStateStore`; represent missing data as `unknown/stale`

## 4. Rendering
- [ ] 4.1 Draw fluid-tinted connected-face quads in the `RenderBridge::ImGuiOverlay` callback (`Render/RenderBridge.cpp`)
- [ ] 4.2 Draw debug text for canonical ID, resolved name, amount/capacity, and stale/unknown state
- [ ] 4.3 Keep overlay read-only; add a regression test proving no mutating request is emitted

## 5. Verify
- [ ] 5.1 `cd cmake-build-debug && ninja -j5`
- [ ] 5.2 `ctest --output-on-failure -j$(nproc)`
- [ ] 5.3 Test connected/disconnected faces, toggle off, non-fluid target, dense pipe, and missing/stale resource state
- [ ] 5.4 Run `graphify update .` after code changes and validate the change strictly

## 6. Explicit scope
- [x] 6.1 Do not add direct client-to-PipeNetwork requests or client-side fluid simulation
- [x] 6.2 Do not add resource filters or separate overlay-specific resource IDs
- [ ] 6.3 Keep network-wide highlighting and cable/heat overlays out of this change
- [ ] 6.4 Keep all protocol changes limited to the existing server-authoritative resource-state channel; do not expose internal PipeNetwork topics to the client.
