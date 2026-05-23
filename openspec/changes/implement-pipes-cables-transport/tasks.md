## 1. Item Pipes
- [ ] 1.1 Register item_pipe, fluid_pipe, dense_item_pipe, dense_fluid_pipe block IDs
- [ ] 1.2 Implement isPipeBlock() with actual block list
- [ ] 1.3 Item pipe BFS graph (separate from energy graph)
- [ ] 1.4 PushItemToPipe: machine output → pipe
- [ ] 1.5 Item movement: 1 block/tick along pipe path
- [ ] 1.6 Insert into machine: pipe → input slot

## 2. Fluid Pipes
- [ ] 2.1 Fluid pipe BFS graph
- [ ] 2.2 Fluid movement along pipes
- [ ] 2.3 Connect to machine FLUID_IN/FLUID_OUT roles

## 3. Energy Cables
- [ ] 3.1 Voltage tier checking (ULV/LV/MV/HV)
- [ ] 3.2 Cable overheat detection and explosion
- [ ] 3.3 Cable loss per block
- [ ] 3.4 Transformers: step-up/down between tiers

## 4. Persistence
- [ ] 4.1 Item buffer persistence via EntityStateStore
