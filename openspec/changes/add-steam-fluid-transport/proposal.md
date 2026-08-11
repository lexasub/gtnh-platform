# Change: Steam Fluid Transport (Machines in the Fluid Pipe Network)

## Why

Fluid transport machinery is fully built (BFS, `fluid.consume.request`,
`fluid.flow` events, `FluidFlowHandler` with FluidStorage / STEAM-energy delivery,
`fluid.consume.response` subscription) — but **machines do not participate**:

- A pipe does not attach to a boiler. Energy machines register in `PipeNetwork` via
  `handleNodeUpdate` → `addNodeWithId(id, x, y, z, 1)` → the `default:` case sets
  `fluidCapacity = 0` → the `connectNodeNeighbors` gate (`fluidCapacity > 0`) never
  creates a pipe↔machine edge. The boiler publishes only `energy.node.update`
  (STEAM source), never `fluid.node.update`.
- Steam machines (`steam_macerator`, `steam_compressor`, `steam_extractor`,
  `steam_mixer`, `bronze_alloy_smelter`) **do not work at all**: `MachineSystem` has
  no `EnergyType::STEAM` branch, so they never request energy (or fluid).
- The `fluid.consume.response` handler in `SimCoreMessageHandler` is a stub (trace
  log only) — no consumer ever gets the delivered amount.

Target: make steam flow **through fluid pipes** from boilers to steam machines,
using the existing transport layer. User decision: **water is not needed** for steam
crafting this iteration — no water tank, no `FluidStorage` for boilers, no water
requests.

## What Changes

- **`PipeNetworkService::handleFluidNodeUpdate`** — when the node already exists
  (energy machine registered first with `fluidCapacity = 0`), upgrade its fluid
  capacity so the masked `connectNodeNeighbors` scan (added in
  `update-pipe-fluid-face-mask`) creates pipe↔machine edges. Without this, energy
  machines can never carry fluid regardless of how many fluid updates they publish.
- **`BoilerSystem`** (heat boiler `1110:01:1`) — publish `fluid.node.update`
  (fluid_id = steam, amount = `steam.steam_stored`, is_source = true) via a new
  `FluidClient` dependency, mirroring the existing `energy.node.update` publish.
- **`GeneratorSystem`** (solid boiler `1110:01:0`) — publish `fluid.node.update`
  (steam, is_source = true) from the STEAM `EnergyStorage`, same pattern.
- **`MachineSystem`** — new `EnergyType::STEAM` branch: publish `fluid.node.update`
  (steam, sink/neutral) so the machine node gets pipe edges, then
  `sendFluidRequest(steam)`; add `pendingFluidConsumes_` + `onFluidConsumeResponse`
  that credits `energy.current` from `FluidConsumeResp` (mirrors the HEAT/ELECTRICITY
  `pendingConsumes_` pattern).
- **`SimCoreMessageHandler`** — wire `fluid.consume.response` to
  `machineSystem->onFluidConsumeResponse(consumed)` (replace the stub).
- **`FluidFlowHandler`** — Case 2b: drain `SteamOutputComponent.steam_stored` (heat
  boiler stores steam there, not in a STEAM `EnergyStorage`); Case 2 (STEAM
  `EnergyStorage`) already covers the solid boiler.
- **`main.cpp` / `spawnECSSystems`** — pass `FluidClient` into `BoilerSystem`,
  `GeneratorSystem`, `MachineSystem` constructors.

## Impact

- Affected specs: `pipes-cables-transport`, `heat-management`
- Affected code:
  - `src/services/pipe_network/PipeNetworkService.cpp` — `handleFluidNodeUpdate`
    (fluid-capacity upgrade for pre-existing machine nodes)
  - `src/services/simulation_core/ECS/Systems/BoilerSystem.{h,cpp}` — fluid publish
  - `src/services/simulation_core/ECS/Systems/GeneratorSystem.{h,cpp}` — fluid publish
  - `src/services/simulation_core/ECS/Systems/MachineSystem.{h,cpp}` — STEAM branch,
    pending fluid consumes, response handler
  - `src/services/simulation_core/ECS/Reactors/FluidFlowHandler.cpp` — SteamOutput drain
  - `src/services/simulation_core/Network/SimCoreMessageHandler.cpp` — fluid response
  - `src/services/simulation_core/main.cpp` — wiring
- Out of scope:
  - Water for boilers / `FluidStorage` machines / creative water source (separate bd issue)
  - Data-driven fluid profile in `machines.yaml` + `MachineInfo` (separate bd issue)
  - Client-side flange rendering from pipes toward machines (cosmetic, separate task)
  - Fluid-type fidelity in `handleFluidConsumeRequest` (pre-existing: source matching
    ignores `req->fluid_id()`; water requests could drain steam — tracked separately)
