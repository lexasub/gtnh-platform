## 1. Specification
- [x] 1.1 Add `Machine Role and Network Topology Semantics` requirement to `heat-management` (document current behaviour)
- [x] 1.2 Confirm `make-boiler-water-free` boiler deltas remain consistent after the refactor; archive it before this change's implementation

## 2. Design helpers (MachineRegistry)
- [x] 2.1 Add `IsHeatSource(block_id)` / `IsHeatSink(block_id)` derived from `energy_in`/`energy_out` + `EnergyStorage.type`
- [x] 2.2 Remove `IsConsumer`/`IsProducer`, the `MachineRole` enum, and the `role` member

## 3. AdjacencyTransferSystem
- [x] 3.1 Replace `role == PRODUCER` / `role != CONSUMER` checks with `IsHeatSource` / `IsHeatSink`
- [x] 3.2 Delete the `//TODO fix read real info` comment at line 45

## 4. Registry + YAML + harness registrations
- [x] 4.1 Remove `role:` parsing from `MachineRegistry::LoadFromYaml` and the `role` member
- [x] 4.2 Remove `role:` from `data/registry/machines.yaml`
- [x] 4.3 Remove hardcoded `MachineRole` args in `main.cpp` and `GameClient.cpp` registrations

## 5. Tests + validation
- [x] 5.1 Update `test_ecs_systems.cpp` role assertions → topology assertions
- [x] 5.2 Rebuild (`ninja simcored_test`) and run `ctest` (expect green)
- [x] 5.3 `openspec validate refactor-machine-role-semantics --strict`
- [x] 5.4 Purge `MachineRole` references from the boiler requirements when the refactor lands

## 6. Coordination
- [ ] 6.1 Rebase after other agents' `SimulationEngine.cpp` / `core.fbs` / `PipeNetwork.*` work lands (standing note — not a blocker for this change)
