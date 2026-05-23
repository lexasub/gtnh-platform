# Change: Implement Pipes & Cables Transport

## Why
Item pipes, fluid pipes, and energy cables are the core transport layer between machines. CableGraph is wired on the server side and PipeMeshBuilder on the client, but the actual block registration, pipe network routing, and gameplay mechanics are missing.

## What Changes
- Register item_pipe, fluid_pipe block IDs in MachineRegistry
- Implement `isPipeBlock()` (currently always false)
- Item pipe BFS graph (separate from energy)
- PushItemToPipe: machine output → pipe
- Item movement: 1 block/tick
- Insert into machine: pipe → input slot
- Fluid pipe BFS graph
- Voltage tier checking for cables
- Cable overheat/explosion
- Cable loss calculations
- Transformers

## Impact
- Affected specs: pipes-cables-transport (new)
- Affected code: pipe_network (BFS, item/fluid graphs), game_client (PipeMeshBuilder), chunk_store (block registration), simulation_core (machine I/O)
