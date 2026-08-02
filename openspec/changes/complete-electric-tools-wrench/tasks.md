## 1. Client Raycast Face Detection (B3)

**Status**: ⚠️ Partial — core G-key + raycast + face detection already implemented in `InteractionSystem.cpp:76-98`. Remaining gaps: `itemId` not sent, no wrench-in-hand check, no client-side `ToolActionResp` handler.

**Context**: `Raycaster::GetTargetedBlock()` (`src/services/game_client/RenderLib/Utils/Raycaster.h:17-19`) returns face normals via `outFaceX/Y/Z`. `NetClient::SendToolAction()` (`src/services/game_client/Network/NetClient.cpp:663-672`) serializes `Protocol::ToolAction` with player_id, ToolActionType, pos + face (no itemId). `InputBinder::registerDefaults()` already has `BindHeld(GLFW_KEY_G, "wrench_cycle")` (`InputBinder.cpp:34`). `InteractionSystem::Update()` already handles `"wrench_cycle"` with full raycast + face index conversion + `SendToolAction(WRENCH_CYCLE)` (`InteractionSystem.cpp:76-98`).

- [x] 1.1 G key binding (`GLFW_KEY_G`) — already done in `InputBinder::registerDefaults()` (`InputBinder.cpp:34`)
- [x] 1.2 Raycast + face detection + `SendToolAction(WRENCH_CYCLE)` — already done in `InteractionSystem::Update()` (`InteractionSystem.cpp:76-98`)
- [ ] 1.3 Add `itemId` param to `NetClient::SendToolAction()` and `ToolAction` FlatBuffers table — server needs to know which item was used
- [ ] 1.4 Add wrench-in-hand check in `InteractionSystem`: only send WRENCH_CYCLE if held item is a wrench (itemId 95+)
- [ ] 1.5 Handle `kToolActionResp` (GatewayMsg::kToolActionResp=14) in client: update machine UI or show failure toast

**Done check**: Press G while looking at a machine with wrench in hand → client sends ToolAction frame (with itemId) → server WrenchHandler::cycleFace fires.

---

## 2. Client Machine Texture Update on Side Config Change

**Status**: ❌ Not implemented

**Context**: Server already publishes `Protocol::MachineConfigUpdated` on `"world.machine.config.updated"` topic. Client must subscribe and update machine face textures.

- [ ] 2.1 Add `"world.machine.config.updated"` topic handler in client network layer (similar to existing `BlockEntityUpdate` handling in `NetClient.h:66-67`)
- [ ] 2.2 On receiving `MachineConfigUpdated` at position (x,y,z):
      - Find the machine block in world render state
      - Update face textures to match new `side_config[6]` roles per face
      - Trigger mesh rebuild for that block position
- [ ] 2.3 Wire `ToolActionRespCallback` to confirm the texture update was server-authoritative

**Done check**: Server cycling side_config → event published → client machine face textures update in <1s.

---

## 3. PipeNetwork BFS Respect Side Config Roles

**Status**: ❌ Not implemented

**Context**: `CableGraph::rebuildGraph()` (`CableGraph.cpp:43-93`) BFS traverses all 6-adjacent cable nodes. Machine→cable connections registered via `registerGenerator()` / `registerMachine()` (`CableGraph.h:49-51`) don't filter by `MachineComponent::side_config`. CableGraph can't currently read MachineComponent (it's in PipeNetwork service, not SimulationCore).

**Architecture decision**: PipeNetwork subscribes to `"world.machine.config.updated"` topic. When side_config changes at a machine position, PipeNetwork updates its per-machine face role cache. RPC to SimulationCore avoided — event-driven is cleaner.

- [x] 3.1 Architecture decided: (C) PipeNetwork subscribes to `"world.machine.config.updated"`, caches side_config per position
- [ ] 3.2 Add `"world.machine.config.updated"` topic handler in `PipeNetworkService::onRouterMessage()` — parse `MachineConfigUpdated`, store `(x,y,z) → side_config[6]` in local map
- [ ] 3.3 Modify `CableGraph::registerMachine()` to accept `sideConfig[6]` param: `registerMachine(entityId, x, y, z, sideConfig[6])`
- [ ] 3.4 Modify `rebuildGraph()` BFS: when traversing machine→cable adjacency, consult cached side_config per position — skip if face role != ENERGY
- [ ] 3.5 Modify `findAdjacentCable()` to accept `sideConfig[6]` param, return 0 if face role != ENERGY
- [ ] 3.6 Update `PipeNetworkService` registration calls to pass side_config from cached map

**Done check**: Machine with side_config face set to INPUT on NORTH face → cables connected only to that face transmit energy/items.

---

## 4. Item EnergyStorage for Tools

**Status**: ⚠️ Partial — `ItemEnergyStorage.h` already exists with `TOOL_ENERGY_DEFS`, `getToolEnergy()`, `setToolEnergy()`, `consumeToolEnergy()`. Energy stored in `ItemStack.meta`. Gap: `DrillSystem::phaseEnergyCheck()` uses machine-level `EnergyStorage` ECS component, not `ItemEnergyStorage`.

**Architecture notes**: Existing tools (`TOOL_ENERGY_DEFS` in BatteryBufferSystem) track energy via item meta field. Battery buffer charging already works. Gaps:
- DrillSystem doesn't check item-level energy before mining
- No visual discharge feedback on client

- [x] 4.1 `ItemEnergyStorage` struct and helper functions — already done in `ItemEnergyStorage.h` (`getToolEnergy`, `setToolEnergy`, `consumeToolEnergy`, `TOOL_ENERGY_DEFS`)
- [ ] 4.2 Wire `DrillSystem` energy check: before starting spiral BFS, call `consumeToolEnergy()` on the held drill ItemStack; abort if insufficient energy
- [ ] 4.3 Handle `kToolActionResp` (success=false, reason="out_of_energy") on client — show warning toast

**Done check**: Tool with low charge → drill mining stops after energy depleted → battery buffer recharges when placed in slot.

---

## 5. Server-Side Wrench Action Cooldown

**Status**: ❌ Not implemented

**Context**: `InteractionSystem::Update()` sends WRENCH_CYCLE each frame G is held because `InputState` lacks edge detection. Server must deduplicate rapid requests.

- [ ] 5.1 Add cooldown map in `WrenchActionHandler`: `std::unordered_map<uint64_t, uint64_t> lastActionTick_` keyed by `(playerId << 32) | packPos(x,y,z) | face`
- [ ] 5.2 On `ToolAction(WRENCH_CYCLE)`, skip if < 4 ticks (200ms at 20Hz) since last action at same position+face
- [ ] 5.3 Reset cooldown on successful `cycleFace()`

**Done check**: Holding G sends WRENCH_CYCLE once per 200ms max, not every frame.

---

## Already Completed

- [x] 6.1 `WrenchHandler::cycleFace()` — server-side face cycling (`WrenchHandler.cpp:21-83`)
- [x] 6.2 EntityStateStore persistence of side_config on change (`WrenchHandler.cpp:52-69`)
- [x] 6.3 `publishMachineConfigUpdatedEvent()` on `"world.machine.config.updated"` (`RouterEventPublisher.cpp:147-161`)
- [x] 6.4 Battery buffer block registration (104=battery_buffer_lv, 105=battery_buffer_mv, 106=battery_buffer_hv, 107=charger)
- [x] 6.5 `BatteryBufferSystem::tick()` charges tools via `TOOL_ENERGY_DEFS` (`BatteryBufferSystem.cpp:7-44`)
- [x] 6.6 `NetClient::SendToolAction()` — client→server ToolAction frame (`NetClient.cpp:663-672`)
- [x] 6.7 `WrenchActionHandler::handle()` — parses `ToolAction`, calls `cycleFace()`, publishes `ToolActionResp` on `"player.tool.action.response"` (`WrenchActionHandler.cpp:10-29`)
- [x] 6.8 `InputBinder` G key → `"wrench_cycle"` binding — done in `InputBinder::registerDefaults()` (`InputBinder.cpp:34`)
- [x] 6.9 `InteractionSystem` raycast + face detection + `SendToolAction(WRENCH_CYCLE)` — done in `InteractionSystem::Update()` (`InteractionSystem.cpp:76-98`)
- [x] 6.10 `ItemEnergyStorage.h` with `getToolEnergy()`/`setToolEnergy()`/`consumeToolEnergy()`/`TOOL_ENERGY_DEFS` — done (`ItemEnergyStorage.h`)
