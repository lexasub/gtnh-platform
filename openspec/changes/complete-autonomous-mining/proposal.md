# Change: Complete Autonomous Mining

## Why
DrillSystem MVP is implemented (DrillComponent, DrillSystem, spiral BFS, mining progress, output buffer, energy consumption). Remaining gaps: persistence, item pipe integration, client UI, and multidimension support.

## What Changes
- Persist drill state in EntityStateStore (position, progress, output buffer)
- Wire drill output buffer to ItemPipe network for auto-ejection
- Client UI: drill machine window with progress bar, energy, output slots
- Multi-dimension support (drill can operate in any dimension)
- SpatialIndex integration (instead of direct GetBlock calls)

## Impact
- Affected specs: autonomous-mining (new)
- Affected code: entity_state_store (drill persistence), pipe_network (item pipe output), game_client (drill UI), simulation_core (DrillSystem multi-dim)
