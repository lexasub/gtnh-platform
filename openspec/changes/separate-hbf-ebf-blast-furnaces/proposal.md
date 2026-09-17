# Change: Separate HBF and EBF blast-furnace semantics

## Why

The current blast-furnace prototype is named EBF but is configured as a HEAT/HU consumer. GTNH distinguishes an Electric Blast Furnace (EBF), powered by EU, from a Hot Blast Furnace (HBF), powered by heat. The repository has no HBF implementation and its EBF recipes must consume dust rather than ore directly.

## What Changes

- Define separate EBF (EU) and HBF (HEAT/HU) machine classes and multiblock controllers.
- Preserve shared blast-furnace structure, coil, hatch, item I/O, and persistence behavior.
- Make EBF use the physical ENERGY hatch endpoint and correlated EU responses.
- Add HBF heat-powered recipes and execution policy.
- Correct blast-furnace recipes to use canonical dust inputs and ingot outputs.
- Add unit and headless coverage for both machine families.

## Impact

- Affected services: SimulationCore, Gateway integration tests, and registry/recipe data.
- Affected data: `items.csv`, `machines.yaml`, `ebf.yaml`, new `hbf.yaml`.
- Backward compatibility: legacy runtime IDs remain accepted only through an explicit compatibility mapping while canonical IDs become authoritative.
- Non-goals: new multiblock geometry, transformer tiers, typed EU ResourcePort migration, and GUI redesign.
