# Change: Complete Electric Tools & Wrench

## Why
Electric tools (drills, battery buffers) and wrench machine-side-config are mostly implemented but have remaining gaps: client machine texture on side_config change, PipeNetwork side_config routing, DrillSystem item-level energy check, and missing `itemId` in tool action protocol.

## What Changes

### Already Implemented
- `WrenchHandler::cycleFace()` — server-side face cycling + EntityStateStore save + `world.machine.config.updated` publish.  
  File: `src/services/simulation_core/Actions/WrenchHandler.cpp:21-83`
- `RouterEventPublisher::publishMachineConfigUpdatedEvent()` — publishes `Protocol::MachineConfigUpdated` on `"world.machine.config.updated"` topic.  
  File: `src/services/simulation_core/Network/RouterEventPublisher.cpp:147-161`
- `BatteryBufferSystem::tick()` — charges tools via `TOOL_ENERGY_DEFS` if PipeNetwork energy connected.  
  File: `src/services/simulation_core/ECS/Systems/BatteryBufferSystem.cpp:7-44`
- Battery buffer blocks 104-107 registered.
- `InputBinder::registerDefaults()` — G key bound to `"wrench_cycle"` (`InputBinder.cpp:34`)
- `InteractionSystem::Update()` — handles `"wrench_cycle"` held action: raycast → face detection → `SendToolAction(WRENCH_CYCLE)` (`InteractionSystem.cpp:76-98`)
- `ItemEnergyStorage.h` — `TOOL_ENERGY_DEFS`, `getToolEnergy()`, `setToolEnergy()`, `consumeToolEnergy()` (`src/services/simulation_core/ECS/components/ItemEnergyStorage.h`)

### Needs Implementation
- **`itemId` in ToolAction protocol** — `NetClient::SendToolAction()` signature lacks `itemId` parameter. Server needs item type to validate wrench usage. Must add to `ToolAction` FlatBuffers table, client call, and server `cycleFace()`.
  Files: `src/protocol/core.fbs` (ToolAction table), `src/services/game_client/Network/NetClient.cpp:663-672`, `src/services/simulation_core/Actions/WrenchHandler.cpp:21-83`
- **Client machine texture update on side_config change** — client must subscribe `"world.machine.config.updated"` and update machine face textures. Currently no handler.  
  Topic: `"world.machine.config.updated"` → `Protocol::MachineConfigUpdated`
- **PipeNetwork BFS respect side_config** — `CableGraph::rebuildGraph()` BFS checks only cable adjacency by position, never filters by face roles. Architecture: PipeNetwork subscribes to `"world.machine.config.updated"` to receive side_config changes, then filters BFS adjacency per-face role.
  Files: `src/services/pipe_network/CableGraph.cpp`, `src/services/pipe_network/PipeNetworkService.cpp`
- **DrillSystem item energy check** — `DrillSystem::phaseEnergyCheck()` uses machine-level `EnergyStorage` ECS component, not `ItemEnergyStorage`. Drill must consume from tool's item-scoped energy via `consumeToolEnergy()`.
  Files: `src/services/simulation_core/ECS/components/ItemEnergyStorage.h`, `src/services/simulation_core/ECS/Systems/DrillSystem.cpp:134-144`
- **Server-side action cooldown** — `InteractionSystem` sends WRENCH_CYCLE each frame G is held (edge detection missing in InputState). Server must deduplicate per `playerId + pos + face` with ~200ms cooldown.
  Files: `src/services/simulation_core/Actions/WrenchActionHandler.cpp:10-29`

## Impact
- Affected specs: electric-tools-wrench (new capability)
- Affected code:
  - `src/protocol/core.fbs` — add `itemId` to `ToolAction` table
  - `src/services/game_client/` — InteractionSystem (wrench-in-hand filter), texture subscription
  - `../../../../src/services/simulation_core/Actions/handTool/WrenchHandler.cpp` — itemId param, permissions check
  - `../../../../src/services/simulation_core/Actions/handTool/WrenchActionHandler.cpp` — cooldown
  - `src/services/simulation_core/ECS/Systems/DrillSystem.cpp` — item energy check
  - `src/services/pipe_network/CableGraph.cpp` — BFS side_config filtering, config update subscription
