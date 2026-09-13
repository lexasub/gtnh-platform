## ADDED Requirements
### Requirement: Machine Steam Transport via Fluid Pipes
Machines SHALL participate in the fluid pipe network as steam sources or steam consumers: steam-producing machines (boilers) SHALL publish `fluid.node.update` so pipe↔machine edges form, and steam-consuming machines (`EnergyType::STEAM`) SHALL request steam via `fluid.consume.request` and credit the delivered amount to their `EnergyStorage.current`.

**References:**
- `src/services/pipe_network/PipeNetworkService.cpp` — `handleFluidNodeUpdate` (fluid-capacity upgrade for pre-existing machine nodes), `handleFluidConsumeRequest`
- `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp` — heat boiler publishes steam fluid source
- `src/services/simulation_core/ECS/Systems/GeneratorSystem.cpp` — solid boiler publishes steam fluid source
- `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` — `EnergyType::STEAM` branch, `pendingFluidConsumes_`, `onFluidConsumeResponse`
- `src/services/simulation_core/Network/SimCoreMessageHandler.cpp` — `fluid.consume.response` → `onFluidConsumeResponse`
- `src/services/simulation_core/ECS/Reactors/FluidFlowHandler.cpp` — SteamOutputComponent drain (Case 2b)
- `src/protocol/pipe_network.fbs` — `FluidNodeUpdate`, `FluidConsumeReq/Resp`, `FluidFlowEvent`

#### Scenario: Energy machine node upgraded with fluid capacity
- **GIVEN** a machine registered in PipeNetwork as an energy node (`fluidCapacity = 0`)
- **WHEN** the machine publishes `fluid.node.update` on topic `fluid.node.update`
- **THEN** `handleFluidNodeUpdate` upgrades the existing node's fluid capacity via `setNodeFluid`
- **AND** `connectNodeNeighbors` creates pipe↔machine edges across open pipe faces

#### Scenario: Boiler publishes steam as a fluid source
- **GIVEN** a boiler machine (`1110:01:0` or `1110:01:1`) with stored steam
- **WHEN** the boiler's system ticks and steam is available
- **THEN** `fluid.node.update` is published with fluid_id = steam (`1111:11:1`), is_source = true
- **AND** adjacent fluid pipes with an open face attach to the boiler node

#### Scenario: Steam machine requests steam through a fluid pipe
- **GIVEN** a steam machine (`EnergyType::STEAM`) with `energy.current < recipe cost`
- **AND** the machine's node is edge-connected to a boiler source through fluid pipes
- **WHEN** `MachineSystem` runs the STEAM branch
- **THEN** `fluid.consume.request` is published for steam on topic `fluid.consume.request`
- **AND** the boiler's steam pool is drained and the machine's `energy.current` is credited with the consumed amount via `fluid.consume.response`

#### Scenario: Steam machine runs a recipe on delivered steam
- **GIVEN** a steam machine that received steam via a fluid pipe
- **WHEN** `energy.current >= recipe.energy_cost`
- **THEN** the recipe progresses and `energy.current` is reduced by the energy cost each tick

#### Scenario: Heat boiler steam drained from SteamOutputComponent
- **GIVEN** a heat boiler (`1110:01:1`) whose steam is stored in `SteamOutputComponent`
- **WHEN** a `FluidFlowEvent` delivers its steam to a consumer
- **THEN** `FluidFlowHandler` drains `steam_stored` (Case 2b)
- **AND** the boiler re-publishes the updated fluid node state
