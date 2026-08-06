# Tasks: Add Game Modes

## 1. Protocol
- [ ] 1.1 Add `PlayerMode` enum (SURVIVAL=0, CREATIVE=1, ADVENTURE=2, SPECTATOR=3) to
      `src/protocol/core.fbs`, mirroring the client `GameMode` values
- [ ] 1.2 Add `SetGameModeReq` (player_id, mode) and `GameModeChanged` (player_id, mode)
      tables + `GatewayPayload` union entries (27/28) in `src/protocol/gateway.fbs`
- [ ] 1.3 Add `GatewayMsg` constants to C++ (`gateway.h` / `NetClient.h`) — keep in sync
      with fbs (known staleness issue, gateway.fbs:22-27)
- [ ] 1.4 Regenerate FlatBuffers, verify build compiles

## 2. Permission matrix
- [ ] 2.1 Define capability lookup (canFly, noclip, canBreak, canPlace, infiniteItems)
      per `GameMode` — in `Common/Inventory.h` or a small `GameModePermissions` helper
- [ ] 2.2 Flip default `InventoryState::gameMode` from CREATIVE to SPECTATOR
      (`Common/Inventory.h:60`)

## 3. Movement (PlayerController)
- [ ] 3.1 `PlayerController`: position, velocity, onGround; per-mode physics
- [ ] 3.2 Fly physics: port current `Camera::Update` movement (SPECTATOR/CREATIVE)
- [ ] 3.3 Walk physics: gravity (~24 blocks/s²), jump, axis-separated AABB sweep vs
      solid blocks via `World::GetBlockAt`, 0.5-block step-up, clamp to loaded chunks
- [ ] 3.4 `Camera`: read pose from `PlayerController`; keep look/orient/frustum
- [ ] 3.5 Wire `PlayerController` + mode into `GameClient::Update` (order: mode →
      movement → interaction raycast)

## 4. NEI gating
- [ ] 4.1 `NeiPanel`: block spawn when mode lacks infiniteItems (disable clicks, show
      "spawning disabled" state)

## 5. Interaction
- [ ] 5.1 `InteractionSystem`: place consumes slot only when mode == SURVIVAL
      (currently consumes always); break/place enabled in all modes (dev behavior)
- [ ] 5.2 Send `SetGameModeReq` on mode switch (from ConsoleWindow or GameClient)

## 6. Server minimal handling
- [ ] 6.1 Gateway: relay `SetGameModeReq` on a game-mode topic
- [ ] 6.2 SimulationCore: store per-player mode, publish `GameModeChanged` echo back to
      the client (no permission checks — dev phase)
- [ ] 6.3 Client: on `GameModeChanged`, log only (already applied locally)

## 7. Tests
- [ ] 7.1 Permission matrix tests (each mode × capability)
- [ ] 7.2 Collision unit tests (fall lands, wall stops, step-up)
- [ ] 7.3 NEI gate test (no spawn when infiniteItems=false)
- [ ] 7.4 Protocol frame-parse test for `SetGameModeReq` / `GameModeChanged`
- [ ] 7.5 Integration: `/gamemode 0` → movement + NEI behavior change; `/gamemode 3`
      → current spectator behavior restored
