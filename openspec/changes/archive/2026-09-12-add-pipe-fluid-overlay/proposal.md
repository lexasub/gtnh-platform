# Change: Add Pipe Fluid Overlay

## Why
In GTNH, looking at a pipe shows which faces are connected — a key UX affordance for routing fluid/steam lines. The current client renders this for the wrench (per-face connectable grid) but provides no persistent, dedicated way to inspect fluid pipe connections at a glance. A toggleable client-side overlay that highlights the connected faces of the targeted fluid pipe brings GTNH-style fluid-network visibility to the platform without touching the protocol or simulation.

## What Changes
This is a **client-facing debug overlay** change. Topology inspection remains client-only, but live buffer values MUST come from the server-authoritative resource-state channel; the client must not query PipeNetwork directly or infer amounts from mesh state.

- Add a toggle keybind `toggle_pipe_fluid_overlay` (default key `L`).
- When toggled on and the player's highlight targets a fluid pipe block (`FLUID_PIPE` or `DENSE_FLUID_PIPE`), the client renders a fluid-tinted translucent overlay on the pipe's **connected faces**.
- Show debug text for the targeted pipe when authoritative state is available: canonical packed fluid/item ID, resolved name, amount, capacity, and stale/unknown status. This is needed to distinguish “steam is not produced” from “steam is produced but stuck in a pipe.”
- Connection mask is computed with the existing `PipeMeshBuilder::detectConnections` (same logic the mesh builder uses), so the overlay matches actual rendered connections.
- Rendering reuses the existing `WrenchOverlay` ImGui screen-space projection (`ImGuiOverlay::DrawOverlay`).
- Internal PipeNetwork registration/transaction messages remain server-only. Any pipe buffer debug state is a separate validated client-facing snapshot applied through the existing client state-store boundary.

## Impact
- Affected specs: `pipes-cables-transport`
- Affected code (all `src/services/game_client/`):
  - `UI/Core/ActionHandler.h` / `.cpp` — new action + toggle state
  - `UI/Core/InputBinder.cpp` — default keybind `L`
  - `RenderLib/Common/RenderAPI.h` — `FrameExt` fields (`showPipeFluidOverlay`, `pipeFluidConnectable[6]`, `pipeFluidIsDense`) (`FrameExt` lives in renderlib; `Render/RenderBridge.h` only includes it)
  - `GameClient.cpp` — compute overlay mask from `detectConnections` each frame when toggled
  - `Render/RenderBridge.cpp` — draw fluid-tinted face quads in the static `RenderBridge::ImGuiOverlay` overlay callback
  - `Network/ResourceBufferStateStore.h` / `.cpp` — existing authoritative buffer-state store, read by the overlay (no schema change)
- Out of scope: protocol, server/sim, network-wide pipe highlighting (see design Out of Scope).
