## Context

SimulationCore owns machine buffers and PipeNetwork owns transport topology. Gateway exposes ACKs, block-entity snapshots, resource-buffer snapshots, and read-only fluid pipe contents, but not internal energy/fluid flow topics.

## Decisions

- Keep steam input and EU output as separate turbine component buffers.
- Use existing `FluidClient`/`PipeEnergyClient` topic contracts and existing owner-side response routing.
- Treat the first integration test as a bounded probe; exact topology and per-tick accounting require direct Router telemetry or focused C++ tests.
- Use registry IDs already allocated in `items.csv`; do not infer rechargeable item IDs from machine block IDs.

## Non-goals

- Multiblock formation or persistence.
- GUI-only assertions.
- Claiming Gateway placement ACKs prove hidden PipeNetwork edges.
