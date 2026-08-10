## 1. Protocol
- [x] 1.1 `src/protocol/core.fbs`: add optional `message: string;` to `ToolActionResp`
- [x] 1.2 `src/protocol/pipe_network.fbs`: add `enum PipeWrenchGuidance : uint8 { NOT_A_PIPE = 0, CONNECT_PIPES = 1, CONNECT_TO_MACHINE = 2, CONNECTED = 3 }`
- [x] 1.3 `src/protocol/pipe_network.fbs`: add `table PipeWrenchAction { player_id: uint64; pos: Vec3i (required); face: uint8; }`
- [x] 1.4 `src/protocol/pipe_network.fbs`: add `table PipeWrenchResp { player_id: uint64; pos: Vec3i (required); guidance: PipeWrenchGuidance; node_id: uint64; }`
- [x] 1.5 Regenerate FlatBuffers C++ stubs (`flatc` / build)

## 2. PipeNetwork
- [x] 2.1 Subscribe topic `pipe.wrench.action` in `PipeNetworkService.cpp`
- [x] 2.2 Implement handler: locate node at position (position-keyed `pipe_nodes_` map), evaluate connection state, publish `PipeWrenchResp` on `pipe.wrench.response`
- [x] 2.3 Connectivity evaluation: pipe-node neighbors via spatial adjacency / edges; adjacent machine blocks via registered machine nodes or the `(x,y,z) → side_config` cache
- [x] 2.4 Guidance mapping: no pipe neighbor + no machine → `CONNECT_PIPES`; pipe neighbor(s) but no machine → `CONNECT_TO_MACHINE`; adjacent machine → `CONNECTED`; no node at position → `NOT_A_PIPE`
- [x] 2.5 Unit tests in `pipe_network_test.cpp`: isolated pipe, pipe+pipe, pipe adjacent machine, non-pipe position

## 3. SimulationCore
- [x] 3.1 Add `ItemId::isPipe()` constexpr prefix-range helper in `common/ItemId.h` (pipes = `1111:10` sub-range, packed `0xF800..0xF803`); add `isCable()`/`isFluid()` companions; refactor `SimulationEngine::isInfraBlock` (`SimulationEngine.cpp:10-17`) and `PipeBlockIds.h` to use them; refactor client `BlockRenderRegistry` pipe checks to `ItemId::isPipe`
- [x] 3.2 `WrenchActionHandler`/`WrenchHandler`: classify target — ECS machine entity → existing `cycleFace`; else async `BlockRepository::getBlock` → pipe → pipe flow; else `not_a_machine`
- [x] 3.3 Publish `PipeWrenchAction` on `pipe.wrench.action` for pipe targets (reuse cooldown key)
- [x] 3.4 New `PipeWrenchResponseHandler` subscribing `pipe.wrench.response`: compose guidance text and publish `ToolActionResp(success=true, message=...)` on `player.tool.action.response`
- [x] 3.5 Guidance texts: `CONNECT_PIPES` → "Pipe is not connected. Place adjacent pipes to build a network."; `CONNECT_TO_MACHINE` → "Pipe has no machine connection. Place the pipe next to a machine to attach it."; `CONNECTED` → "Pipe is connected to a network (N segments)." (N = connected component node count from `PipeWrenchResp.node_id` + PipeNetwork)
- [x] 3.6 Wire handler + subscriptions in `SimCoreMessageHandler.cpp`

## 4. Client
- [x] 4.1 `NetClient.cpp`: pass `ToolActionResp.message` through the callback
- [x] 4.2 `GameClient.cpp` ToolActionRespCallback: show non-empty `message` as a toast (promote `ToastMessage` from per-window to a global HUD overlay)
- [x] 4.3 `RenderBridge.cpp`: GT-style wrench overlay on the highlighted block when holding a wrench and targeting a wrenchable block — corner crosses + direction bars (up/down/left/right + top/bottom), raycast face bar preselected
- [x] 4.4 Pipe direction bars: render connectable directions from client-local neighbor blocks (`GetBlockAt` + `ItemId::isPipe` / machine classification); dim or omit non-connectable directions
- [x] 4.5 Click handling (`InteractionSystem`): face-specific wrench cycling delivered via the existing G-key raycast-face override; click-on-bar alias scoped out (server returns guidance only — no edge mutation in this change)

## 5. Verification
- [x] 5.1 Build (`ninja` in `cmake-build-debug`) and run `ctest --output-on-failure -j$(nproc)`
- [ ] 5.2 Manual: isolated pipe → toast "connect pipes"; pipe+pipe → toast "connect to machine"; pipe next to machine → toast "connected"; machine wrench still cycles side_config; no overlay without wrench
- [ ] 5.3 Cross-change: coordinate with `make-boiler-water-free` — with both changes applied, a pipe wrenched adjacent to a boiler (`1110:01:0`/`1110:01:1`, STEAM source nodes) returns `CONNECTED`; confirm no topic/schema collision at build time
