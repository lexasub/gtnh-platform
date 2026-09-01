## ADDED Requirements
### Requirement: Pipe Fluid Overlay
The game client SHALL provide a toggleable fluid-pipe debug overlay that highlights the connected faces of the fluid pipe block currently targeted by the player's highlight ray. The overlay SHALL be enabled by a `toggle_pipe_fluid_overlay` action (default key `L`) and SHALL compute the per-face connection mask using the same logic as pipe mesh construction (`PipeMeshBuilder::detectConnections`), so the overlay matches the rendered connections. Fluid pipes include `FLUID_PIPE` and `DENSE_FLUID_PIPE`. The overlay SHALL be read-only: it MUST NOT query PipeNetwork directly or send registration, drain, or consume requests. When authoritative resource state exists, the overlay SHALL show the canonical resource/item ID, resolved name, amount, capacity, and stale/unknown status; absent state MUST be shown as unknown rather than fabricated zero.

**References:**
- `src/services/game_client/UI/Core/ActionHandler.h` / `.cpp` — `DoTogglePipeFluidOverlay`, `PipeFluidOverlayOn()`
- `src/services/game_client/UI/Core/InputBinder.cpp` — default bind `L` → `toggle_pipe_fluid_overlay`
- `src/services/game_client/RenderLib/Common/RenderAPI.h` — `FrameExt.showPipeFluidOverlay`, `pipeFluidConnectable[6]`, `pipeFluidIsDense` (`FrameExt` lives here; `Render/RenderBridge.h` only includes it)
- `src/services/game_client/GameClient.cpp` — frame mask computation via `detectConnections`
- `src/services/game_client/Render/RenderBridge.cpp` — `RenderBridge::ImGuiOverlay` fluid face quads + debug text
- `src/services/game_client/Render/PipeMeshBuilder.h` — `detectConnections`, `PipeType`
- `src/services/game_client/Network/ResourceBufferStateStore.h` / `.cpp` — client-side authoritative buffer-state store (canonical packed ID, amount, capacity; epoch/sequence staleness gates, `FindAt()` = `nullptr` when no snapshot)
- `src/common/GatewayMsg.h` — `kResourceBufferState = 47` (client-facing resource-state message type)
- `src/common/ResourceBufferStateCodec.h` — topic `sim.resource.buffer.state`, serialize/parse
- `src/services/simulation_core/Network/ResourceBufferStatePublisher.cpp` — server publish path

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

#### Scenario: Debug state identifies the targeted pipe
- **GIVEN** the overlay is enabled and a targeted fluid pipe has a current server snapshot
- **WHEN** the frame renders
- **THEN** the overlay shows the canonical resource/item ID, resolved name, amount, and capacity
- **AND** the displayed state is read from the client state store

#### Scenario: Missing debug state is explicit
- **GIVEN** the overlay is enabled and no current server snapshot exists for the targeted pipe
- **WHEN** the frame renders
- **THEN** the overlay shows `unknown` or `stale`
- **AND** it does not display a fabricated zero amount

#### Scenario: No mutating protocol impact
- **GIVEN** the overlay is enabled and rendering
- **WHEN** a fluid pipe is highlighted
- **THEN** no registration, drain, or consume request is sent
- **AND** the server/simulation state is unchanged
- **AND** internal PipeNetwork messages are not exposed directly to the client
