# Tasks: Headless thermal power chain

## 1. Machine activation
- [x] 1.1 Register turbine and battery-buffer variants in machines.yaml.
- [x] 1.2 Create ECS components for turbine and battery-buffer machine initialization.
- [x] 1.3 Wire resource publication and accepted-response handling.

## 2. Transport systems
- [x] 2.1 Implement steam-to-EU turbine conversion with bounded buffers.
- [x] 2.2 Ensure electricity sink nodes and buffer state are observable.
- [x] 2.3 Verify heat/fluid/cable graph connectivity and cleanup.

## 3. Tests
- [x] 3.1 Add focused C++ turbine and battery-buffer regression tests.
- [x] 3.2 Add headless Gateway thermal-chain probe with correlated ACKs.
- [ ] 3.3 Add state-transition assertions for steam, EU, batteries, and 32-EU/t processing.

## 4. Verification
- [ ] 4.1 Build affected targets incrementally in cmake-build-debug.
- [ ] 4.2 Run focused unit and integration tests with PipeNetwork active.
- [ ] 4.3 Validate OpenSpec, diff whitespace, and update graphify.
