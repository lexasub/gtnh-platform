# Change: Add a headless thermal-to-electric power-chain test

## Why

The headless Gateway client currently verifies isolated machines but cannot exercise a complete thermal power chain. The registry and SimulationCore lack an activated steam-turbine and battery-buffer path, so a test that only checks placement ACKs would give false confidence.

## What Changes

- Register steam turbine and battery-buffer machine variants already allocated in `items.csv`.
- Add a bounded SimulationCore steam-turbine conversion system and activate battery buffers as electricity sinks.
- Add focused unit coverage for conversion and buffer behavior.
- Add a deterministic integration probe covering multiple generators, boilers, heat/fluid pipes, turbines, buffers, and a 32-EU/t electric consumer.

## Impact

- Affected services: SimulationCore and PipeNetwork.
- Affected tests: SimulationCore unit tests and headless Gateway integration tests.
- Persistence and multiblock scenarios remain outside this change.
