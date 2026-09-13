# Change: Wrench-on-Pipe Guidance and GT-Style Wrench Overlay

## Why
The wrench currently only cycles machine `side_config`; wrenching a pipe silently fails
(`not_a_machine`, invisible to the player — the client only logs it). Players get no
feedback about pipe connection state, and PipeNetwork never learns that a wrench was
used on a pipe. GTNH reference behavior: the wrench shows a face overlay with direction
hints so the player knows which way to turn/connect.

## What Changes
- **SimCore** detects a `WRENCH_CYCLE` ToolAction targeting a pipe block and routes it
  through a dedicated pipe-wrench flow instead of machine side-config cycling. Target
  classification is server-authoritative (ECS machine entity first, then ChunkStore query).
- **New internal pub/sub pair** (SimCore → PipeNetwork → SimCore):
  - `pipe.wrench.action` — `PipeWrenchAction { player_id, pos, face }`
  - `pipe.wrench.response` — `PipeWrenchResp { player_id, pos, guidance, node_id }` with
    `PipeWrenchGuidance` enum: `NOT_A_PIPE`, `CONNECT_PIPES`, `CONNECT_TO_MACHINE`, `CONNECTED`
- **PipeNetwork** evaluates the wrenched pipe's connection state (registered node, pipe
  neighbors, adjacent machine blocks) and reports guidance. No graph mutation in this change.
- **SimCore** composes guidance text and returns it to the player via an extended
  `ToolActionResp.message` field (reuses existing per-player routing on
  `player.tool.action.response`; no new GatewayMsg/topic/gateway changes).
- **Client** renders a GT-style wrench overlay on the targeted wrenchable block (machine or
  pipe): corner crosses + edge direction bars, raycast face highlighted; for pipes, bars
  mark connectable directions from client-local neighbor data.
- Machine side-config wrench behavior is unchanged. Cables are out of scope.

## Impact
- Affected specs: `electric-tools-wrench`, `pipes-cables-transport`
- Affected code:
  - `src/protocol/core.fbs` — `ToolActionResp.message` (additive field)
  - `src/protocol/pipe_network.fbs` — `PipeWrenchAction`, `PipeWrenchGuidance`, `PipeWrenchResp`
  - `src/common/ItemId.h` — prefix classification helpers (`isPipe`/`isCable`/`isFluid`), removes duplicated id lists
  - `src/services/simulation_core/Actions/handTool/WrenchActionHandler.cpp` + new `PipeWrenchResponseHandler`
  - `src/services/simulation_core/Network/RouterEventPublisher.cpp` — publish helper
  - `src/services/pipe_network/PipeNetworkService.cpp` — subscribe `pipe.wrench.action`, handler, connectivity evaluation
  - `src/services/game_client/GameClient.cpp` / `Network/NetClient.cpp` — toast on `ToolActionResp.message`
  - `src/services/game_client/Render/RenderBridge.cpp` — GT-style wrench overlay
- Shared zone touched: `src/protocol/` (two additive `.fbs` changes)
- Coordinated with `make-boiler-water-free` (pending): boiler STEAM source nodes feed the
  `CONNECTED` pipe evaluation; topics and schema fields are disjoint (see design.md)
