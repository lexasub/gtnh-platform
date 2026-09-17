# Design: Scaled energy-hatch multiblock thermal integration

## Context

SimulationCore already forms LCR pattern 3 and publishes the controller's legacy EU node. The pattern also declares an ENERGY hatch, but the current hatch model treats that role as structural and the LCR system publishes from the controller position. PipeNetwork's supported EU path is the legacy energy node/consume contract; typed resource draining currently supports FLUID only.

## Decisions

- Use LCR pattern 3 as the consumer multiblock. It is already an electric multiblock with item input/output and ENERGY hatch declarations.
- Use `hatch_energy_input_lv` from `items.csv` (`1110:111:14`, packed `0xEE0E`) as the physical LV energy hatch. Keep legacy placeholder item/fluid hatch compatibility for existing tests, but stop using the battery-buffer ID range as an energy-hatch ID.
- Keep the ECS owner as the LCR controller entity. The energy node remains owned by that entity for response handling, while its advertised position is the physical ENERGY hatch position.
- Keep the first scaled fixture LV-only. Use four parallel generator→boiler→fluid-pipe→turbine branches and four battery buffers feeding one shared LV cable network and the LCR hatch.
- Use the existing uncorrelated legacy energy response only with one LCR consumer. Battery responses remain isolated by preserving their existing node/FIFO behavior; a future protocol change can add request/node identity for multiple consumers.
- Formation is complete only after the controller is placed last and the Gateway receives the existing multiblock-created event. The test asserts physical hatch presence and state transitions, not unsupported ChunkStore `mb_id` persistence.

## Risks and mitigations

- The current `EnergyConsumeResp` has no consumer identity. Keep one LCR sink in this scenario and document multi-LCR routing as follow-up work.
- Existing pattern coordinates use legacy structural IDs and placeholders. Centralize canonical hatch IDs in `PatternLibrary.h` and add focused tests to prevent collisions with battery buffers.
- CableGraph currently attaches an owner to one adjacent cable. Place the hatch directly beside the cable trunk and assert the hatch-position node rather than relying on controller adjacency.
- Gateway does not forward internal energy topics. Use Gateway snapshots for user-visible state and a direct Router observer only for registration/consume transitions where required.
