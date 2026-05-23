# Remaining Work — 0-foundation-energy-fluids

This epic has been **split** into:

1. **Archive** (this directory) — L1 primitives that were completed
2. **Continuation spec** — see `doc/EPICS/0-foundation-energy-fluids/0-foundation-energy-fluids.md`

## What was implemented (archived)

### L1 Energy — Done
- **EnergyStorage ECS component** (`simulation_core/ECS/components/EnergyStorage.h`): capacity, current, maxInput, maxOutput, tier; addEnergy/consumeEnergy helpers
- **MachineSystem energy consumption** (`simulation_core/ECS/Systems/MachineSystem.cpp`): checks `energy.current >= energy_cost`, subtracts on each tick
- **BlockEntityUpdate energy field**: publishes `energy.current` to client for progress bar display
- **EnergyStorage FlatBuffers schema** (`protocol/`): generated Go code exists

### L1 PipeNetwork — Done (library-level)
- **PipeNetwork C++ service** (`pipe_network/`): complete graph data structures
  - `PipeNode{id, x, y, z, block_id, energyBuffer/Capacity, fluidBuffer/Capacity/Id, isSource/isSink}`
  - `PipeEdge{fromNode, toNode, resistance}`
  - `PipeNetwork{id, nodeIds, totalEnergy, totalFluid, fluidId, isActive}`
  - `PipeNetworkManager`: addNode/removeNode, addEdge/removeEdge, BFS discoverNetwork, rebuildNetworks
  - `distributeEnergy()` / `distributeFlow()` / `distributeFluid()` — implemented with source/sink proportional distribution
- **main.cpp**: stub (prints message, returns 0 — no MessageRouter integration yet) → теперь полный сервис, подключён к MessageRouter (см. continuation spec)

### L1 Protocol — Done
- **FluidTank** struct in `core.fbs` (fluid_id, amount, color)
- **FluidStack** struct in `core.fbs`
- **MachineFluidTank** in `machine_state.fbs` (fluid_id, amount, capacity)
- **fluids** field in MachineState message

## What moved to continuation spec

Everything in the **continuation spec** at `doc/EPICS/0-foundation-energy-fluids/`:

- Energy generation (adding EU to machines)
- PipeNetwork → MessageRouter integration
- PipeNetwork → SimulationCore integration
- Fluid gameplay (tanks, fluid recipes, GUI)
- L2 wires/cables, fluid pipes, generators, transformers (deferred)

## Original tasks

Preserved in `archive/0-foundation-energy-fluids/tasks/`.
