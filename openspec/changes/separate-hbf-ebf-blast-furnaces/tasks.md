## 1. Canonical data

- [ ] 1.1 Allocate and promote distinct canonical EBF/HBF controller, casing, and coil IDs.
- [ ] 1.2 Add EBF and HBF machine classes with EU and HEAT domains.
- [ ] 1.3 Correct `ebf.yaml` to dust→ingot EU recipes and add `hbf.yaml` dust→ingot HEAT/HU recipes.
- [ ] 1.4 Add RecipeManager tests for class/domain/input/output compatibility.

## 2. Shared multiblock execution

- [ ] 2.1 Parameterize shared blast-furnace hatch IO, progress, reservation, and output logic.
- [ ] 2.2 Implement EBF EU policy with physical ENERGY hatch endpoint.
- [ ] 2.3 Implement HBF HEAT/HU policy without EU consumption.
- [ ] 2.4 Resolve coil aggregation, heat thresholds, and muffler/energy-hatch presence rules.

## 3. Runtime and protocol wiring

- [ ] 3.1 Replace provisional EBF registration with canonical EBF/HBF registrations and compatibility migration.
- [ ] 3.2 Register both systems and ensure generic MachineSystem does not process them.
- [ ] 3.3 Keep node-correlated EU responses routed to the correct furnace owner.
- [ ] 3.4 Remove furnace endpoints and pending requests on hatch/controller destruction.

## 4. Verification

- [ ] 4.1 Add focused unit tests for EBF EU and HBF HU execution, stalls, and exact outputs.
- [ ] 4.2 Add headless formation and recipe tests for both furnace families.
- [ ] 4.3 Keep direct thermal and LCR energy-hatch regressions green.
- [ ] 4.4 Run full CTest, Go tests, strict OpenSpec validation, diff checks, and graphify update.
