# Change: Implement Multiblocks L2

## Why
L1 multiblock prototype exists (electrolyser pattern matching, controller registry) but L2 gameplay is missing: SpatialIndex, generic pattern library, EBF/Large Boiler/LCR specialized ticks, dissociation, hatch detection.

## What Changes
- Implement SpatialIndex (R-tree + Octree)
- Create generic pattern matching library (replace hardcoded ELECTROLYSER_PATTERN)
- Implement EBF tick (heating coils, heat requirement, muffler)
- Implement Large Boiler tick (firebox, water→steam, multi-size)
- Implement LCR tick (fluid chemistry)
- Dissociation detection (break multiblock when anchor is removed)
- Hatch detection (input/output/energy/fluid hatches)
- sim.multiblock.* topics on MessageRouter

## Impact
- Affected specs: multiblocks-l2 (new)
- Affected code: spatial_index (new R-tree), simulation_core (ECS systems, pattern matching), message_router (new topics), entity_state_store (multiblock persistence)
