# Change: Add verification for headless thermal power chain

## Why
Core implementation of the headless thermal-to-electric power chain is complete. Remaining work focuses on verification, state-transition assertions, and CI validation to ensure deterministic behavior.

## What Changes
- Add state-transition assertions for steam, EU, batteries, and 32-EU/t processing
- Build affected targets incrementally in cmake-build-debug
- Run focused unit and integration tests with PipeNetwork active
- Validate OpenSpec, diff whitespace, and update graphify

## Impact
- Affected specs: thermal-power-chain (references existing spec from add-headless-thermal-power-chain)
- Affected code: SimulationCore unit tests, headless Gateway integration tests, CI validation
