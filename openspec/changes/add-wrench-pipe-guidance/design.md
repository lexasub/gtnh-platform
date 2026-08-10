## Context

Wrench flow today (SimCore `WrenchActionHandler` → `WrenchHandler::cycleFace`): a
`WRENCH_CYCLE` ToolAction looks up an ECS entity with `MachineComponent` at the target
position and cycles `side_config[face]`, then publishes `world.machine.config.updated`.
Pipe blocks have no machine entity, so wrenching a pipe returns
`{false, "no_machine_at_position"}` — the client logs the failure but shows nothing.

PipeNetwork owns pipe topology: `PipeNetworkManager` (nodes/edges), position-keyed
`pipe_nodes_` map for spatial adjacency, and a `(x,y,z) → side_config` cache fed by
`world.machine.config.updated`. All SimCore↔PipeNetwork communication is MessageRouter
pub/sub (`*.node.update`, `*.check.request/response`, `world.blocks.changed`); there is
no direct RPC.

There is no general server→player message channel (no chat topic/table/gateway relay).
The only text the server can send to the wrenching player today is `ToolActionResp.error`
via `player.tool.action.response`, and the client does not display it.

## Goals / Non-Goals

- Goals:
  - SimCore detects a wrench on a pipe (server-authoritative) and gives the player
    actionable guidance.
  - PipeNetwork learns about the wrench event and reports the pipe's connection state.
  - Client shows a GT-style face/direction overlay while holding a wrench.
- Non-Goals:
  - Edge mutation (toggle connect/disconnect) on wrench — PipeNetwork only evaluates and
    reports in this change.
  - General chat/notification channel — a `PlayerMessage`-style pipeline is a follow-up.
  - Cables / wrench-on-cable behavior in this change.
  - **Cable reuse note:** `ItemId::isPipe()` is shipped together with `isCable()`/`isFluid()`
    companions so a future wrench-on-cable change reuses the same classification source of
    truth without an extraction refactor. Cables stay out of the wrench flow for now.

## Decisions

- **Decision: Guidance via extended `ToolActionResp.message`, not a new player-message channel.**
  `ToolActionResp` already flows per-player (gateway routes `player.tool.action.response`
  to the connection that sent the action) and is exactly the message type the wrench flow
  already answers with. Adding an optional `message:string` field is additive in FlatBuffers
  and touches no gateway code. A general server→player channel is deferred until a second
  consumer needs it.
  - Alternatives considered: new `PlayerMessage` table + `GatewayMsg` constant + gateway
    relay + client HUD pipeline. Rejected for this change: ~3× surface for the same UX.

- **Decision: Round-trip `pipe.wrench.action` → `pipe.wrench.response`.**
  SimCore stays the owner of player communication ("SimCore говорит юзеру"); PipeNetwork
  stays free of UI concerns and answers with a machine-readable guidance enum.
  - Alternatives considered: PipeNetwork publishing guidance text directly. Rejected —
    couples PipeNetwork to player messaging and message wording.

- **Decision: Server-authoritative target classification by ItemId prefix.**
  `ToolAction` carries no target block id (only held `item_id`), so SimCore cannot trust
  the client to say "this is a pipe". Flow: ECS machine-entity lookup first (sync, existing
  `cycleFace` path); if absent, query ChunkStore via the block repository (same pattern as
  `MachineInteractHandler` lazy-init) and classify the block id.
  - Block ids are prefix-encoded (`common/ItemId.h`): `"1111:10:0"` packs to `0xF800` — 6-bit
    prefix `111110` (62) shifted into the top bits. Pipes occupy the `1111:10` sub-range
    (`0xF800..0xF803`), cables `1111:01` (`0xF400..0xF405`), fluids `1111:11` (`0xFC00..`).
    Classification is a constexpr range check: `isPipe(id)` =
    `id >= ItemId::pack("1111:10:0") && id < ItemId::pack("1111:11:0")`.
  - Add `ItemId::isPipe()` (+ `isCable()`/`isFluid()` companions) in `common/ItemId.h` — its
    NOLINT section (`ItemId.h:171-174`) is explicitly reserved for category helpers.
    This removes existing duplication: `SimulationEngine::isInfraBlock`
    (`SimulationEngine.cpp:10-17`, 10 hardcoded `pack()` calls), `PipeBlockIds.h`,
    `CableTypes.h`, and the client's `BlockRenderRegistry` pipe checks all become one
    shared, zero-cost source of truth.
  - `data/registry/pipes.csv` holds pipe *properties* (tier, flow_rate, items_per_sec) and
    is currently loaded by nothing. Structural classification stays prefix-based by design;
    loading pipes.csv for property data is a separate follow-up, not part of this change.

- **Decision: GT overlay is in-world markup and a face selector.**
  The overlay is not a UI window — it is a GT-style in-world markup rendered at the
  highlighted block (`RenderBridge::ImGuiOverlay`, reusing the existing wireframe corner
  projection at `RenderBridge.cpp:136-170`). It is a screen-space rectangle silhouette of
  the targeted block: four direction bars along the silhouette edges (top / bottom / left /
  right) with crosses at the four corners where bars intersect.
  - The four edge bars map to the four screen-facing side faces; clicking a bar sends a
    `WRENCH_CYCLE` for that side face (as today's raycast-face override).
  - The **corner crosses** map to the two depth-axis faces — front / back relative to the
    player camera ("развернуть перед-назад" = turn front-to-back). For a pipe, clicking a
    cross means "connect backward/forward" on that depth face. Crosses are visually distinct
    from the edge bars. The overlay MUST be drawn on the UI/HUD (not silently skipped).
  - The raycast face is still highlighted as preselected when no bar/cross is clicked.
  - The bars are interactive: a click is hit-tested against the screen-space bar regions and
    mapped to a block face (same face enum already used for `WRENCH_CYCLE`); the click then
    sends `WRENCH_CYCLE` with that face, overriding the raycast face. The face travels in
    `PipeWrenchAction.face`, so PipeNetwork evaluates connectivity on the clicked side.
  - Direction data for pipe bars is client-local and cosmetic (`GetBlockAt` on the 6
    neighbors, pipe/machine classification via the new `ItemId::isPipe` helper). No per-frame
    server round trip. The server guidance text remains authoritative for what the player
    should do.

- **Decision: No pipe-graph mutation in this change.**
  Wrench-on-pipe is detect + evaluate + guide. `PipeNetworkManager::addEdge/removeEdge`
  hooks are the natural next step but are deliberately not specced here ("пока по трубам" —
  phase 1 is awareness and guidance).

## Risks / Trade-offs

- Async ChunkStore query adds latency to the wrench response → the existing 200 ms cooldown
  key (player + pos + face) is reused for the pipe flow to prevent spam.
- `ToolActionResp` has no `player_id`; routing is per-connection. Acceptable: the response
  returns to the connection that sent the action, which is the wrenching player.
- New topics `pipe.wrench.action` / `pipe.wrench.response` must be added to
  `PipeNetworkService` subscriptions and the new SimCore handler — both services already
  follow this subscribe/handle pattern.

## Migration Plan

Additive protocol fields only (`ToolActionResp.message`); existing machines keep the
side-config flow unchanged. No schema breaking changes; no data migration.

## Related Changes

- `make-boiler-water-free` (pending, `openspec/changes/make-boiler-water-free/`): rewires both
  boilers to produce STEAM — `1110:01:0` coal→STEAM via `GeneratorSystem`, `1110:01:1` HEAT→STEAM
  via `BoilerSystem` — and publishes STEAM source nodes through the existing
  `pipeClient_->publishNodeUpdate(...)` (`is_source=true`, `type=STEAM`).
  - **No conflicts**: that change reuses existing node-update topics; this change adds new
    `pipe.wrench.action` / `pipe.wrench.response` topics. Affected capability specs are disjoint
    (`heat-management` vs `electric-tools-wrench` + `pipes-cables-transport`).
  - **Complementary**: boilers become registered machine nodes, so a pipe wrenched adjacent to a
    boiler evaluates as `CONNECTED` via the machine-node check (`PipeWrenchGuidance::CONNECTED`).
    The boiler work is the machine side of the "трубу к машине подключить" story this change
    enables.
  - **Shared zone**: both add additive FlatBuffers fields (`BlockEntityUpdate` steam fields vs
    `ToolActionResp.message` + `PipeWrenchAction`/`PipeWrenchResp`) — different tables, no
    collision; one flatc regeneration covers both.

## Open Questions

- **Resolved — `CONNECTED` reports network node count.** Yes. `PipeWrenchResp.node_id` is
  already present; PipeNetwork also computes the connected component size at evaluation time and
  SimCore's `CONNECTED` guidance text includes the node count (e.g. "Pipe is connected to a
  network (N segments).").
- **Resolved — overlay bars + crosses.** Four edge bars (top/bottom/left/right) + crosses at
  the four corners. Corner crosses map to the depth-axis faces (front/back relative to the
  player) and MUST render on the UI; clicking a cross connects on that depth face.
