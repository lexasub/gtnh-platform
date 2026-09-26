# Tasks: Add Pipe Fluid Overlay

## 1. OpenSpec + scaffold
- [ ] 1.1 Write `proposal.md`, `tasks.md`, `design.md`, delta `spec.md`
- [ ] 1.2 Run `openspec validate add-pipe-fluid-overlay --strict`

## 2. Toggle action + keybind
- [ ] 2.1 Add `DoTogglePipeFluidOverlay` / `PipeFluidOverlayOn()` to `ActionHandler` + register action
- [ ] 2.2 Add default bind `L` → `toggle_pipe_fluid_overlay` in `InputBinder::registerDefaults`

## 3. Frame data + computation
- [ ] 3.1 Add `FrameExt` fields for the overlay in `RenderBridge.h`
- [ ] 3.2 Compute `showPipeFluidOverlay` / `pipeFluidConnectable[6]` / `pipeFluidIsDense` in `GameClient.cpp` via `detectConnections`

## 4. Rendering
- [ ] 4.1 Draw fluid-tinted connected-face quads in `ImGuiOverlay::DrawOverlay`

## 5. Verify
- [ ] 5.1 `cd cmake-build-debug && ninja -j5`
- [ ] 5.2 `ctest --output-on-failure -j$(nproc)`
