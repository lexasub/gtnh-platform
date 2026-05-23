# Tasks: Transport — Pipes & Cables

## A. Item Pipes

- [ ] item_pipe block_id в MachineRegistry
- [ ] isPipeBlock() для item_pipe в PipeNetworkService
- [ ] PipeNetwork: item graph (отдельный от energy)
- [ ] PushItemToPipe protocol (PipeNetwork API)
- [ ] Item movement: 1 block/tick через BFS
- [ ] Insert into machine: pipe → input slot
- [ ] Item buffer persistence через EntityStateStore
- [x] **Client: pipe mesh builder** (PipeMeshBuilder + detectConnections + integration) — **✅ done**

## B. Fluid Pipes

- [ ] fluid item_id в registries (water, steam, sulfuric_acid)
- [ ] PipeNetwork: fluid graph (отдельный)
- [ ] BoilerSystem → pipe → machine flow
- [ ] Infinite water source placeholder
- [ ] Gravity: fluid flows downward
- [ ] Client: fluid pipe visuals
- [ ] RecipeManager: fluid item_id рецепты

## C. Energy Cables & Voltage

- [ ] Cable block_id per tier в MachineRegistry (tin, copper, gold, alu, tungsten, platinum)
- [ ] isCableBlock() в PipeNetworkService
- [ ] Voltage tier checking при BFS routing
- [x] **Cable overheat → explosion при превышении tier** (CableGraph::tick + calculateOverheat + CableExplodedEvent) — **✅ done**
- [x] **Cable loss: energy_loss = distance * loss_per_block** (effectiveVoltage + CableGraph::tick per hop) — **✅ done**
- [ ] Transformer block: step up/down, face config
- [x] **Client: cable mesh builder** (CableMeshBuilder + tier colors + detectConnections) — **✅ done**
- [ ] Client: overvoltage warning UI
- [ ] Client: explosion effect при перегрузке
