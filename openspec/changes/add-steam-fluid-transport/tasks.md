# Tasks

> **Status 2026-08-11**: ALL tasks done and verified (1.1–1.7 diff reviewed against spec, full build clean, 16/16 ctest passed; 1.9 test added, simcored_test 82/82 green incl. new `MachineSystem_steam_machine_requests_fluid`).

## 1. Implementation (verified call sites)

### 1.1 PipeNetworkService — fluid-capacity upgrade
- [x] Implemented — `handleFluidNodeUpdate` existing-node branch, matches spec snippet exactly.
- **File**: `src/services/pipe_network/PipeNetworkService.cpp`
- **Function**: `handleFluidNodeUpdate` (line ~569). Existing-node branch = `else` at line ~589 (after `auto it = protocol_to_mgr_.find(protocol_id);`).
- **Fix**: in the existing-node branch, BEFORE the trailing `connectNodeNeighbors(mgr_id, x, y, z, /*sourceMeta=*/0, /*isItem=*/false, /*sourceIsPipe=*/false)` call (line ~615-616, must keep running unconditionally):
  ```cpp
  const auto* nn = network_manager_.getNode(mgr_id);
  if (nn && nn->fluidCapacity <= 0) {
      network_manager_.setNodeFluid(mgr_id, update->amount(), update->capacity(),
                                    update->fluid_id(), update->is_source(), update->is_sink());
  }
  ```
- `FluidNodeUpdate.fluid_id` exists in `src/protocol/pipe_network.fbs` (line 142) — use `update->fluid_id()`.
- `setNodeFluid(nodeId, fluid, capacity, fluidId, isSource, isSink)` — `PipeNetwork.h:163`.
- Keep the `st.energy/st.capacity/st.is_source/st.is_sink` NodeState assignments as-is. New-node path (created with `BLOCK_ID_FLUID_PIPE`) unchanged.

### 1.2 BoilerSystem — heat boiler publishes steam fluid source
- [x] Implemented — ctor param + publish; heat boiler branch, amounts match energy publish.
- [x] Bugfix 2026-08-11: publish moved OUT of the conversion guard (was `heat_stored > 0 && steam_stored < capacity`) — a cold/full boiler never registered a node, so pipes could not attach. Node publishes (energy + fluid) now fire every tick; conversion + BlockEntityUpdate stay guarded.
- **Files**: `src/services/simulation_core/ECS/Systems/BoilerSystem.{h,cpp}`; ctor `BoilerSystem.h:21` currently `(entt::registry&, shared_ptr<IEventPublisher>, shared_ptr<PipeEnergyClient>)`.
- **Change**: add `std::shared_ptr<FluidClient> fluidClient` param (last). Include `Network/FluidClient.h`, `common/ItemId.h`.
- **Publish** (heat boiler branch, `machine.machine_id == ItemId::pack("1110:01:1")`, after the existing `pipeClient_->publishNodeUpdate(...)` at line ~47-56):
  ```cpp
  if (fluidClient_) {
      fluidClient_->publishNodeUpdate(
          static_cast<uint64_t>(ent), machine.x, machine.y, machine.z,
          ItemId::pack("1111:11:1"),              // steam
          static_cast<int32_t>(steam.steam_stored),
          static_cast<int32_t>(steam.steam_capacity),
          0, static_cast<int32_t>(toConvert), energy.tier,
          true, false);                           // is_source=true, is_sink=false
  }
  ```
- Same amounts as the energy publish (shared pipe-network node state must stay consistent).

### 1.3 GeneratorSystem — solid boiler publishes steam fluid source
- [x] Implemented — ctor param + publish in the `EnergyType::STEAM` block only. Extra guard comment: heat generators must NOT register as steam source (consumer BFS would drain phantom steam).
- [x] Bugfix 2026-08-11: unconditional STEAM node registration (energy + fluid) added at tick start — same cold/idle boiler attachment problem as 1.2. Burn-block publish kept (idempotent double-publish while burning).
- **Files**: `src/services/simulation_core/ECS/Systems/GeneratorSystem.{h,cpp}`; ctor `GeneratorSystem.h:20` currently `(entt::registry&, shared_ptr<IEventPublisher>, shared_ptr<PipeEnergyClient>)`.
- **Change**: add `std::shared_ptr<FluidClient> fluidClient` param (last).
- **Publish** in the existing `EnergyType::STEAM` block (line ~106-…, after `pipeClient_->publishNodeUpdate(...)`):
  ```cpp
  if (fluidClient_) {
      fluidClient_->publishNodeUpdate(
          static_cast<uint64_t>(ent), machine.x, machine.y, machine.z,
          ItemId::pack("1111:11:1"),              // steam
          energy.current, energy.capacity,
          0, energy.maxOutput, energy.tier,
          true, false);                           // is_source=true
  }
  ```

### 1.4 MachineSystem — STEAM branch + fluid consume
- [x] Implemented — STEAM branch matches spec; `pendingFluidConsumes_` + `onFluidConsumeResponse` added (FIFO, clamp-credit via `add_sat`, self-healing: `consumed<=0` drops oldest pending → machine re-requests next tick).
- **Files**: `src/services/simulation_core/ECS/Systems/MachineSystem.{h,cpp}`; ctor `MachineSystem.h:26`.
- **Change**: add `std::shared_ptr<FluidClient> fluidClient` ctor param. Include `Network/FluidClient.h`, `common/ItemId.h`.
- **Branch**: in the energy-shortfall chain (`if (energy.current < recipe->energy_cost)`, line ~213, branches HEAT ~216 / ELECTRICITY ~235 / ROTATION ~254, all end `continue` ~291) add:
  ```cpp
  } else if (energy.type == EnergyType::STEAM) {
      uint64_t node_id = static_cast<uint64_t>(ent);
      auto pit = pendingFluidConsumes_.find(node_id);
      if (pit == pendingFluidConsumes_.end()) {
          int32_t needed = static_cast<int32_t>(recipe->energy_cost);
          if (fluidClient_) {
              // register node with fluid capacity first (pipes attach via fluid.node.update)
              fluidClient_->publishNodeUpdate(
                  node_id, machine.x, machine.y, machine.z,
                  ItemId::pack("1111:11:1"), energy.current, energy.capacity,
                  0, 0, energy.tier, false, true);     // sink/neutral
              fluidClient_->sendFluidRequest(
                  node_id, machine.x, machine.y, machine.z,
                  ItemId::pack("1111:11:1"), needed);
          }
          pendingFluidConsumes_[node_id] = needed;
          spdlog::debug("Steam machine {} at entity {} requested {} steam from PipeNetwork",
                        recipe->id, static_cast<uint32_t>(ent), needed);
      }
  }
  ```
- **Members** (`MachineSystem.h`, near `pendingConsumes_` at line 59): `std::unordered_map<uint64_t, int32_t> pendingFluidConsumes_;` + `void onFluidConsumeResponse(int32_t consumed);`.
- **`onFluidConsumeResponse`** — mirror `onConsumeResponse` (read lines ~440-485): take OLDEST pending entity from `pendingFluidConsumes_` (no node id in `FluidConsumeResp` — oldest-pending is the established pattern), clamp-credit `energy.current` to capacity, erase, clear map when empty.

### 1.5 SimCoreMessageHandler — wire fluid consume response
- [x] Implemented — `fluid.consume.response` branch now calls `machineSystem->onFluidConsumeResponse(resp->consumed())` (guard `!resp || !machineSystem`).
- **File**: `src/services/simulation_core/Network/SimCoreMessageHandler.cpp`, `fluid.consume.response` branch line ~213-217 (currently trace-log stub).
- **Fix**: mirror the energy branch (line ~204-211):
  ```cpp
  } else if (topic == "fluid.consume.response") {
      auto* resp = flatbuffers::GetRoot<Protocol::FluidConsumeResp>(data.data());
      if (!resp || !machineSystem) return;
      machineSystem->onFluidConsumeResponse(resp->consumed());
  }
  ```
- `Deps` already has `fluidClient` (`SimCoreMessageHandler.h:50`) and `machineSystem` (`:59`).

### 1.6 FluidFlowHandler — Case 2b: SteamOutputComponent drain
- [x] Implemented — Case 2b between Case 2 and Case 3, matches spec snippet exactly.
- **File**: `src/services/simulation_core/ECS/Reactors/FluidFlowHandler.cpp`; Case 2 (STEAM `EnergyStorage`) at line ~74-86, Case 3 (blocked) at ~89-91. `fluidClient_` available.
- **Insert Case 2b between Case 2 and Case 3** (heat boiler stores steam in `SteamOutputComponent` — its `EnergyStorage` is HEAT type, Case 2 misses it):
  ```cpp
  // Case 2b: No STEAM EnergyStorage, but SteamOutputComponent — boiler steam pool
  if (auto* soc = reg_.try_get<SteamOutputComponent>(entity)) {
      soc->steam_stored -= amount;
      if (soc->steam_stored < 0) soc->steam_stored = 0;
      if (mc && fluidClient_) {
          fluidClient_->publishNodeUpdate(
              from_node, mc->x, mc->y, mc->z,
              fluid_id, soc->steam_stored, soc->steam_capacity,
              0, 0, 0, true, false);              // is_source=true
      }
      spdlog::trace("FluidFlowHandler: fluid {} x{} drained from SteamOutputComponent at ({},{},{})",
                    fluid_id, amount, x, y, z);
      break;
  }
  ```
- Include `ECS/components/SteamOutputComponent.h`. Fields verified: `steam_stored`, `steam_capacity` (used in `BoilerSystem.cpp:32-41`).

### 1.7 main.cpp wiring
- [x] Implemented — `spawnECSSystems` gains `fluidClient` param; passed to GeneratorSystem, BoilerSystem, MachineSystem ctors.
- **File**: `src/services/simulation_core/main.cpp`.
- `fluidClient` created at line 233. `msgDeps.fluidClient = fluidClient` at line 442 (already done).
- `MachineSystem` ctor call (lines ~367-370): append `fluidClient` as last arg.
- `spawnECSSystems` (declaration lines ~87-90, call at ~380): add `std::shared_ptr<simcore::FluidClient> fluidClient` param; pass to `GeneratorSystem` (line ~94) and `BoilerSystem` (line ~96) ctors as last arg.

### 1.8 Build + tests
- [x] PASSED 2026-08-11: `ninja -j5` full build clean; `ctest --output-on-failure -j$(nproc)` → 16/16 passed (1 disabled: toctou). Includes `simcored_test`, `pipe_network_test`, `integration_test`.
- `cd cmake-build-debug && ninja -j5` (pipenetworkd + simcored_exec targets), then `ctest --output-on-failure -j$(nproc)`.
- Existing boiler/flow test template: `src/services/simulation_core/test/test_ecs_systems.cpp:865-899` (SteamOutputComponent + STEAM EnergyStorage).

### 1.9 Unit test (optional but recommended) — DONE
- [x] Implemented 2026-08-11: `test_MachineSystem_steam_machine_requests_fluid` in `test_ecs_systems.cpp` + `MockFluidClient` spy (requires `virtual` on `FluidClient::publishNodeUpdate`/`sendFluidRequest` in `FluidClient.h` — the only production change). Covers: publish (steam, is_sink) + request (amount = energy_cost) → `onFluidConsumeResponse(20)` credits → self-heal (`consumed<=0` drops pending, no credit without pending). simcored_test: 82/82 passed.
- Note (agent discovery): real `data/recipes/macerator.yaml` steam recipes carry no `eu` → `energy_cost` = 0 → STEAM branch guard can never fire with real data. Test injects a temp-YAML recipe with `eu: 32` instead. Data fix is a follow-up (see section 2).
- Extend `test_ecs_systems.cpp`: steam machine entity (STEAM `EnergyStorage`, needs energy) with `fluidClient` mocked/stubbed → STEAM branch publishes fluid node + sends request → `onFluidConsumeResponse(consumed)` credits `energy.current` → recipe ticks.

## 2. Follow-ups (tracked separately, NOT part of this change)
- bd issue: data-driven fluid profile in `machines.yaml` + `MachineInfo` (fluid_in/fluid_out), `FluidStorage` machines, water tanks, creative water source.
- bd issue: fluid-type fidelity in `handleFluidConsumeRequest` (source matching ignores `req->fluid_id()`; water requests could drain steam).
- bd issue: steam recipes in `data/recipes/macerator.yaml` (and other steam machines) lack `eu` → `energy_cost` = 0 → STEAM branch never fires with real data. Add `eu` (steam cost) to steam recipes.
- Client: render pipe flange toward machines (cosmetic).
