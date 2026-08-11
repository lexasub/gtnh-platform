# Tasks: Init Game Flow

## 1. Protocol
- [x] 1.1 Add `StartScenarioReq` (player_id, scenario_index) and `StartScenarioResp`
      (player_id, scenario_index, success, error, game_mode, quest_book_era) tables to
      `src/protocol/core.fbs`
- [x] 1.2 Add `StartScenarioReq` / `StartScenarioResp` to the `GatewayPayload` union in
      `src/protocol/gateway.fbs`
- [x] 1.3 Add `GatewayMsg::kStartScenarioReq = 31` and `kStartScenarioResp = 32` to
      `gateway.h` and `NetClient.h` (next free after `kGameModeChange = 30`; fbs union indices
      are known-stale, C++ constants govern the wire)
- [x] 1.4 Regenerate FlatBuffers, verify the build compiles

## 2. Server — scenario table (SimulationCore)
- [x] 2.1 New `simulation_core/Scenario/GameScenario.h/.cpp`: `GameScenario` struct
      (index, name, targetMode, giveItems, clearFirst, questBookEra) + static `scenarios()`
      table. Scenario 0 = { clearFirst=true, giveItems=[(22529,1),(30723,1)],
      targetMode=SURVIVAL, questBookEra=VAGRANT }
- [x] 2.2 Add `simulation_core/Scenario/` to the SimulationCore CMake build

## 3. Server — scenario handler
- [x] 3.1 Handler for topic `player.scenario.start` in `SimCoreMessageHandler.cpp`:
      verify `StartScenarioReq` buffer, reject `player_id == 0`, reject
      `scenario_index` out of range (log + no-op)
- [x] 3.2 Execute: `PlayerInventoryStore::setSlots(empty)` → `giveItem(...)` per giveItems →
      `setGameMode(targetMode)` → publish `StartScenarioResp` (success, game_mode,
      quest_book_era) on `player.scenario.start.response`. Do NOT re-publish the
      `player.gamemode.changed` echo (mode travels in the resp)
- [x] 3.3 `main.cpp`: `Subscribe("player.scenario.start")`

## 4. Gateway
- [x] 4.1 Route `kStartScenarioReq` → publish `player.scenario.start` (mirror the
      `kGameModeChange` case at `gateway.cpp:528`)
- [x] 4.2 Subscribe `player.scenario.start.response` → `send_to_client_ctrl_raw(kStartScenarioResp)`
      (mirror `player.gamemode.changed` at `gateway.cpp:420`)
- [x] 4.3 `gateway/main.cpp`: `subscribe("player.scenario.start.response")`

## 5. Client
- [x] 5.1 `NetClient`: `SendStartScenarioReq(player_id, scenario_index)` + resp callback
      (`onStartScenarioResp_`)
- [x] 5.2 `ConsoleWindow.cpp`: `RegisterCommand("startGameScenario", ...)` with strict validation
      matching `/gamemode` (empty arg / non-numeric / index out of range → console error, no
      request sent). `/help` lists it
- [x] 5.3 New `game_client/UI/Windows/player/GameScenario.h/.cpp`: display-only scenario table
      (name, questBookEra, outputMessage) for `/help` + console output; on successful resp:
      apply `gameMode` to `InventoryState::gameMode`, print outputMessage
- [x] 5.4 `QuestBookWindow`: add `SetEra(int eraIndex)`; programmatic open from the scenario resp
      (`uiMgr_->Find<QuestBookWindow>()` → `SetOpen(true)` + `SetEra(era)`). Register
      GameScenario.cpp in `game_client/UI/CMakeLists.txt`

## 6. C4 diagrams
- [x] 6.1 `doc/c4/level4-client-ui-windows.puml`: add ConsoleWindow, QuestBookWindow, GameScenario
      to «Окна игрока» with UIManager edges

## 7. Tests
- [x] 7.1 Protocol frame-parse test for `StartScenarioReq` / `StartScenarioResp`
- [x] 7.2 Server handler test: scenario 0 clears + grants + sets mode; unknown index rejected;
      `player_id == 0` rejected; `player.inventory.update` published before the response
- [x] 7.3 ConsoleWindow arg-validation test (empty / non-numeric / out-of-range → no send)
- [x] 7.4 Integration: `/startGameScenario 0` → SURVIVAL + starter inventory snapshot +
      QuestBook opens on Vagrant (requires running server+client — covered by unit tests 7.1-7.3)
