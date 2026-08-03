## 1. Client Raycast Face Detection (B3)

**Status**: ✅ Complete

**Context**: `Raycaster::GetTargetedBlock()` (`src/services/game_client/RenderLib/Utils/Raycaster.h:17-19`) returns face normals via `outFaceX/Y/Z`. `NetClient::SendToolAction()` (`src/services/game_client/Network/NetClient.cpp:663-672`) serializes `Protocol::ToolAction` with player_id, ToolActionType, pos + face + itemId. `InputBinder::registerDefaults()` has `BindHeld(GLFW_KEY_G, "wrench_cycle")` (`InputBinder.cpp:34`). `InteractionSystem::Update()` handles `"wrench_cycle"` with raycast + face index conversion + wrench-check + `SendToolAction(WRENCH_CYCLE, itemId)`.

- [x] 1.1 G key binding — `InputBinder::registerDefaults()` (`InputBinder.cpp:34`)
- [x] 1.2 Raycast + face detection — `InteractionSystem::Update()` (`InteractionSystem.cpp:76-98`)
- [x] 1.3 `itemId` param — `InteractionSystem` passes `getSelectedBlockId()` to `SendToolAction()` (`InteractionSystem.cpp:93`)
- [x] 1.4 Wrench-in-hand check — skip WRENCH_CYCLE if `heldItem != ITEM_WRENCH` (`InteractionSystem.cpp:79-83`)
- [x] 1.5 `kToolActionResp` handler — `GameClient::subscribeNetClient()` wires `SetToolActionRespCallback` (`GameClient.cpp:102-117`)

**Done check**: Press G while looking at a machine with wrench in hand → client sends ToolAction frame (with itemId) → server WrenchHandler::cycleFace fires.

---

## 2. Client Machine Texture Update on Side Config Change

**Status**: ✅ Complete (alternative approach via ToolActionResp)

**Context**: Server publishes `Protocol::MachineConfigUpdated` on `"world.machine.config.updated"` topic. Client receives `ToolActionResp` via Gateway (forwarded from `player.tool.action.response`). The response contains `all_roles[6]` — sufficient for face texture update. Client triggers mesh rebuild via `MeshManager::OnBlockUpdate()` which re-renders faces using `FaceTextureRegistry::getFaceTexture()`.

- [x] 2.1 `ToolActionResp` already forwarded by Gateway (`gateway.cpp:387`) as `GatewayMsg::kToolActionResp`
- [x] 2.2 `NetClient::OnMessage()` parses `kToolActionResp` and calls `onToolActionResp_` (`NetClient.cpp:360-372`)
- [x] 2.3 `SetToolActionRespCallback` wired in `GameClient::subscribeNetClient()` — on success, triggers `meshMgr_.OnBlockUpdate()` at the targeted position (`GameClient.cpp:102-117`)
- (Note: client does NOT subscribe to `world.machine.config.updated` directly — ToolActionResp provides equivalent data with lower latency)

**Done check**: Server cycling side_config → client receives ToolActionResp → mesh rebuild updates face textures in <1s.

---

## 3. PipeNetwork BFS Respect Side Config Roles

**Status**: ✅ Complete

**Context**: `PipeNetworkService::handleMachineConfigUpdated()` (`PipeNetworkService.cpp:622-649`) already exists, subscribed to `"world.machine.config.updated"` at init (`PipeNetworkService.cpp:128`). Parses `MachineConfigUpdated`, calls `network_manager_.setNodeSideConfig(mgr_id, side_config)` for BFS filtering.

- [x] 3.1 Architecture decided: event-driven, subscribe to `"world.machine.config.updated"`
- [x] 3.2 `handleMachineConfigUpdated()` exists in `PipeNetworkService::onRouterMessage()` dispatcher at line 230-231
- [x] 3.3 Subscription registered: `router_.Subscribe("world.machine.config.updated")` at init line 128
- [x] 3.4 `setNodeSideConfig()` called on PipeNetworkManager — caches per-machine face roles
- [x] 3.5 BFS filtering handled internally by PipeNetworkManager using cached side_config
- [x] 3.6 Event published by `RouterEventPublisher::publishMachineConfigUpdatedEvent()` (`RouterEventPublisher.cpp:148-162`)

**Done check**: Machine with side_config face set to INPUT on NORTH face → PipeNetworkManager filters connections per role.

---

## 4. Item EnergyStorage for Tools

**Status**: ⚠️ Partial — `ItemEnergyStorage.h` exists. `ToolActionResp` callback wired for generic failure handling. DrillSystem energy check deferred — requires inventory architecture.

- [x] 4.1 `ItemEnergyStorage` struct + helpers — `ItemEnergyStorage.h` (`getToolEnergy`, `setToolEnergy`, `consumeToolEnergy`, `TOOL_ENERGY_DEFS`)
- [ ] 4.2 Wire `DrillSystem` energy check — deferred (GTNH-d4v, needs inventory arch)
- [x] 4.3 `ToolActionResp` callback wired — `GameClient.cpp:102-117` handles success/failure

**Done check**: Tool with low charge → drill mining stops after energy depleted → battery buffer recharges when placed in slot. (Long-term: GTNH-d4v)

---

## 5. Server-Side Wrench Action Cooldown

**Status**: ✅ Complete

**Context**: `WrenchActionHandler::handle()` (`WrenchActionHandler.cpp:10-39`) uses `std::chrono::steady_clock` cooldown map. Key = hash(playerId, x, y, z, face) packed into uint64. 200ms threshold.

- [x] 5.1 Cooldown map: `std::unordered_map<uint64_t, TimePoint> lastActionTime_` keyed by `cooldownKey()` hash (`WrenchActionHandler.h:25-27`)
- [x] 5.2 Cooldown check: skip if < 200ms since last action (`WrenchActionHandler.cpp:35-39`)
- [x] 5.3 Cooldown reset: timepoint updated on each successful handle() call

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
