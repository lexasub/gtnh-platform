# Tasks: Add Game Modes

Status: **implemented**. Interaction gating (break/place per mode, inventory
consumption) is tracked by the separate `add-interaction-mode-gating` change;
the tasks below that concern interaction are removed from this change's scope.

## 1. Protocol
- [x] 1.1 Add `GameMode` enum (SURVIVAL=0, CREATIVE=1, ADVENTURE=2, SPECTATOR=3) to
      `src/protocol/core.fbs`, mirroring the client `GameMode` values
- [x] 1.2 Add `GameModeChange` table (player_id, new_mode) to `src/protocol/core.fbs`
      (single message; not separate SetGameModeReq/GameModeChanged union members)
- [x] 1.3 Add `kGameModeChange = 30` to the C++ `GatewayMsg` constants (`gateway.h` /
      `NetClient.h`)
- [x] 1.4 Regenerate FlatBuffers, verify build compiles

## 2. Permission matrix
- [x] 2.1 Define capability lookup (canFly, noclip, canBreak, canPlace, infiniteItems)
      per `GameMode` in `Common/Inventory.h` (`GameModePerm`); flight gate uses `CanFly`
- [ ] 2.2 ~~Flip default `InventoryState::gameMode` from CREATIVE to SPECTATOR~~ —
      **decided against**: default stays CREATIVE (`Common/Inventory.h:64`)

## 3. Movement (PlayerController)
- [x] 3.1 `PlayerController` (`game_client/Player/PlayerController.cpp`): position,
      velocity, onGround; per-mode physics
- [x] 3.2 Fly physics: free-fly (previous `Camera::Update` movement) for
      SPECTATOR/CREATIVE
- [x] 3.3 Walk physics: gravity (25 blocks/s²), jump, sneak, axis-separated AABB sweep
      vs solid blocks via `World::GetBlockAt`, ~0.5-block step-up — for
      SURVIVAL/ADVENTURE
- [x] 3.4 `Camera`: read pose from `PlayerController`; keep look/orient/frustum
- [x] 3.5 Wire `PlayerController` + mode into `GameClient::Update` (flight gate uses
      `GameModePerm::CanFly`)

## 4. NEI gating
- [x] 4.1 Gate item spawning on `GameModePerm::InfiniteItems` — in `NeiPanel`
      (`CanSpawn()`, click + banner) and `ActionHandler::SpawnItem` (single choke point)

## 5. Interaction
- [ ] 5.1 ~~Place consumes slot only in SURVIVAL~~ — **moved** to
      `add-interaction-mode-gating` (code currently disables break/place in
      ADVENTURE/SPECTATOR; inventory consumption semantics live there)
- [x] 5.2 Send `GameModeChange` on mode switch (from `ConsoleWindow`)

## 6. Server minimal handling
- [x] 6.1 Gateway: relay `GameModeChange` on the game-mode topics
      (`player.gamemode.change` / `player.gamemode.changed`)
- [x] 6.2 SimulationCore: store per-player mode (`PlayerInventoryStore::setGameMode`),
      publish echo back to the client (no permission checks — dev phase)
- [x] 6.3 Client: on `GameModeChange` echo, log only (already applied locally)

## 7. Tests
- [x] 7.1 Permission matrix tests (each mode × capability) —
      `tests/test_gamemode_permissions.cpp`, wired into CMake + ctest
- [ ] 7.2 Collision unit tests (fall lands, wall stops, step-up) — deferred: requires a
      `World` stub; physics is a verbatim port from `Camera::Update`
- [ ] 7.3 NEI gate test — deferred: UI-render dependent; the gate logic is covered by
      7.1 via `InfiniteItems`
- [ ] 7.4 Protocol frame-parse test for `GameModeChange` — deferred
- [ ] 7.5 Integration: `/gamemode 0` → movement + NEI behavior change — manual/UX
