# Change: Add Pipe Fluid Overlay

## Why
In GTNH, looking at a pipe shows which faces are connected — a key UX affordance for routing fluid/steam lines. The current client renders this for the wrench (per-face connectable grid) but provides no persistent, dedicated way to inspect fluid pipe connections at a glance. A toggleable client-side overlay that highlights the connected faces of the targeted fluid pipe brings GTNH-style fluid-network visibility to the platform without touching the protocol or simulation.

## What Changes
This is a **client-only** change (game_client service). No protocol, server, or simulation changes.

- Add a toggle keybind `toggle_pipe_fluid_overlay` (default key `L`).
- When toggled on and the player's highlight targets a fluid pipe block (`FLUID_PIPE` or `DENSE_FLUID_PIPE`), the client renders a fluid-tinted translucent overlay on the pipe's **connected faces**.
- Connection mask is computed with the existing `PipeMeshBuilder::detectConnections` (same logic the mesh builder uses), so the overlay matches actual rendered connections.
- Rendering reuses the existing `WrenchOverlay` screen-space geometry (`wrench_overlay` in `src/apps/game_client/Render/WrenchOverlay.h`, drawn via the ImGui overlay path in `WrenchOverlay.cpp`), so the fluid overlay looks like the wrench one.

## Impact
- Affected specs: `pipes-cables-transport`
- Affected code (all `src/services/game_client/`):
  - `UI/Core/ActionHandler.h` / `.cpp` — new action + toggle state
  - `UI/Core/InputBinder.cpp` — default keybind `L`
  - `Render/RenderBridge.h` — `FrameExt` fields (`showPipeFluidOverlay`, `pipeFluidConnectable[6]`, `pipeFluidIsDense`)
  - `GameClient.cpp` — compute overlay mask from `detectConnections` each frame when toggled
  - `Render/ImGuiOverlay.cpp` — draw fluid-tinted face quads in `DrawOverlay`
- Out of scope: protocol, server/sim, network-wide pipe highlighting (see design Out of Scope).
