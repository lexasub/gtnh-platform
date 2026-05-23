# Change: Complete Electric Tools & Wrench

## Why
Electric tools (drills, battery buffers) and wrench machine-side-config are mostly implemented but have remaining gaps: raycast face detection, side_config persistence, client textures, and PipeNetwork routing integration.

## What Changes
- Add wrench raycast face detection on client (B3)
- Save side_config in EntityStateStore
- Publish machine.config.updated topic
- Update client machine textures based on side_config
- Wire PipeNetwork BFS to respect side_config roles
- Register remaining battery buffer blocks (104-107)
- EnergyStorage for item tools

## Impact
- Affected specs: electric-tools-wrench (new)
- Affected code: game_client (raycast, textures), simulation_core (EntityStateStore), pipe_network (BFS routing), entity_state_store (side_config persistence)
