## 1. Item Pipes
- [ ] 1.1 Register item_pipe, fluid_pipe, dense_item_pipe, dense_fluid_pipe block IDs
- [ ] 1.2 Implement isPipeBlock() with actual block list
- [x] 1.3 Item pipe BFS graph (separate from energy graph)
- [x] 1.4 PushItemToPipe: machine output → pipe
- [x] 1.5 Item movement: 1 block/tick along pipe path
- [ ] 1.6 Insert into machine: pipe → input slot

## 2. Fluid Pipes
- [x] 2.1 Fluid pipe BFS graph
- [x] 2.2 Fluid movement along pipes
- [ ] 2.3 Connect to machine FLUID_IN/FLUID_OUT roles

## 3. Energy Cables
- [x] 3.1 Voltage tier checking (ULV/LV/MV/HV)
- [x] 3.2 Cable overheat detection and explosion
- [x] 3.3 Cable loss per block
- [ ] 3.4 Transformers: step-up/down between tiers

## 4. Persistence
- [ ] 4.1 Item buffer persistence via EntityStateStore

## Note: CableGraph (388 lines) and PipeNetworkManager (626 lines) are implemented. Energy BFS, overheat, loss, item/fluid transport work. Gaps: machine inventory insertion, fluid→machine integration, transformers.
