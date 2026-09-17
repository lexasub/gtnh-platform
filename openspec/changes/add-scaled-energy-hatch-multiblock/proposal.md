# Change: Add scaled energy-hatch multiblock thermal integration

## Why

The validated thermal integration currently ends in a standalone LV machine connected through ordinary cables. It does not exercise the multiblock resource boundary or prove that generated EU can reach a physical energy hatch. A larger headless scenario is needed to validate the real machine infrastructure before extending the test to more multiblock types.

## What Changes

- Make the canonical LV energy hatch (`1110:111:14`) detectable and usable by the existing LCR multiblock pattern.
- Route LCR energy consumption through the physical hatch position while retaining the controller as the ECS owner.
- Scale the headless thermal fixture to multiple heat generators, boilers, turbines, battery buffers, fluid pipes, and LV cables.
- Replace the standalone wire-connected consumer with an LCR multiblock and assert formation, hatch presence, EU delivery, and recipe progress.
- Preserve the existing direct thermal-chain test as a regression baseline.

## Impact

- Affected services: SimulationCore, PipeNetwork, and integration test utilities.
- Affected data: canonical multiblock hatch mapping and LV infrastructure fixture.
- Protocol: reuse the existing legacy `EnergyNodeUpdate`/`EnergyConsumeReq` contract; no schema change in this slice.
- Non-goals: typed EU `ResourcePort` support, new voltage tiers, persistence redesign, and non-anchor multiblock retention semantics.
