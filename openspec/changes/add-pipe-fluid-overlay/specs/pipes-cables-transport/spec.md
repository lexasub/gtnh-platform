## ADDED Requirements
### Requirement: Pipe Fluid Overlay
The game client SHALL provide a toggleable, client-side overlay that highlights the connected faces of the fluid pipe block currently targeted by the player's highlight ray. The overlay SHALL be enabled by a `toggle_pipe_fluid_overlay` action (default key `L`) and SHALL compute the per-face connection mask using the same logic as pipe mesh construction (`PipeMeshBuilder::detectConnections`), so the overlay matches the rendered connections. Fluid pipes include `FLUID_PIPE` and `DENSE_FLUID_PIPE`. The overlay SHALL have no effect on the protocol, server, or simulation.

**References:**
- `src/services/game_client/UI/Core/ActionHandler.h` / `.cpp` — `DoTogglePipeFluidOverlay`, `PipeFluidOverlayOn()`
- `src/services/game_client/UI/Core/InputBinder.cpp` — default bind `L` → `toggle_pipe_fluid_overlay`
- `src/services/game_client/Render/RenderBridge.h` — `FrameExt.showPipeFluidOverlay`, `pipeFluidConnectable[6]`, `pipeFluidIsDense`
- `src/services/game_client/GameClient.cpp` — frame mask computation via `detectConnections`
- `src/services/game_client/Render/ImGuiOverlay.cpp` — `DrawOverlay` fluid face quads
- `src/services/game_client/Render/PipeMeshBuilder.h` — `detectConnections`, `PipeType`

#### Scenario: Overlay shows connected faces of a targeted fluid pipe
- **GIVEN** the `toggle_pipe_fluid_overlay` overlay is enabled
- **AND** the player's highlight ray targets a `FLUID_PIPE` block that is connected on the +X and +Y faces
- **WHEN** the frame renders
- **THEN** fluid-tinted quads are drawn on the +X and +Y faces of that block
- **AND** no highlight is drawn on the disconnected faces

#### Scenario: Overlay inactive when toggle off
- **GIVEN** the `toggle_pipe_fluid_overlay` overlay is disabled
- **WHEN** the frame renders while targeting a fluid pipe
- **THEN** no fluid overlay is drawn

#### Scenario: Overlay ignores non-fluid blocks
- **GIVEN** the overlay is enabled
- **WHEN** the player's highlight targets a non-fluid block (e.g. a cable or stone)
- **THEN** no fluid overlay is drawn

#### Scenario: No protocol or server impact
- **GIVEN** the overlay is enabled and rendering
- **WHEN** a fluid pipe is highlighted
- **THEN** no additional protocol messages are sent and the server/simulation state is unchanged
