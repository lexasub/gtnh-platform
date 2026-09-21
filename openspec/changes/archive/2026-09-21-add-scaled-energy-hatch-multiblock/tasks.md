## 1. Registry and hatch model

- [x] 1.1 Add canonical LV energy-hatch ID mapping (`1110:111:14`) and remove the placeholder collision with battery-buffer IDs.
- [x] 1.2 Extend hatch state with physical presence/tier/port details while preserving item hatch slot ordering.
- [ ] 1.3 Add focused tests for LCR ENERGY hatch detection, absent hatch rejection/fallback, tier, and face configuration.
*Moved to add-scaled-energy-hatch-multiblock-verification*

## 2. Energy transport through the hatch

- [x] 2.1 Advertise the LCR's legacy EU node at its physical ENERGY hatch position.
- [ ] 2.2 Ensure PipeNetwork/CableGraph connects the cable endpoint to the hatch position and removes it with the controller.
*Moved to add-scaled-energy-hatch-multiblock-verification*
- [x] 2.3 Route LCR energy requests through the hatch endpoint while preserving controller ECS ownership.
- [x] 2.4 Add focused tests for hatch endpoint registration and accepted EU consumption.

## 3. Scaled headless scenario

- [ ] 3.1 Refactor the thermal fixture into reusable placement and state-observation helpers.
*Moved to add-scaled-energy-hatch-multiblock-verification*
- [x] 3.2 Place multiple generators, boilers, fluid pipes, turbines, LV battery buffers, cables, and an LCR with canonical item/fluid/energy hatches.
- [x] 3.3 Insert fuel, rechargeable cells, and recipe inputs; assert every placement/slot response.
- [x] 3.4 Assert LCR formation event, physical energy hatch, steam/EU/battery transitions, and recipe progress/output.
- [ ] 3.5 Assert hatch/multiblock removal cleanup without claiming unsupported persistence or non-anchor retention.
*Moved to add-scaled-energy-hatch-multiblock-verification*

## 4. Verification and follow-up

- [x] 4.1 Keep the existing direct thermal-chain test green.
- [x] 4.2 Run focused C++ tests, scaled Gateway integration, full ctest, strict OpenSpec validation, diff checks, and graphify update.
- [ ] 4.3 File follow-up for correlated multi-consumer EU responses and typed EU ResourcePort support.
*Moved to add-scaled-energy-hatch-multiblock-verification*
