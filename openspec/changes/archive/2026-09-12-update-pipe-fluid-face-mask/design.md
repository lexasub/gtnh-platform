# Design: Per-Face Connection Masking for Item/Fluid Pipes

## Context
A prior design doc (`docs/pipe-cable-connect-disconnect.md`) assumed item/fluid machine→pipe
edges came from a `connected_nodes` ECS list populated in SimulationCore. Investigation
(`bg_b459eb86`) showed this is **stale**: every item/fluid `publishNodeUpdate` call passes an
empty `connected_nodes`, so the field is `nullptr` and never consumed. Connectivity for item
pipes is instead built by a synchronous neighbor scan inside `PipeNetworkService`, and fluid
connectivity is not built at all. Cables are already mask-aware via `CableGraph` + a
synchronous `pipe_meta_` cache populated from `BlockChangedEvent`.

## Goals / Non-Goals
- Goals: make item/fluid per-face disconnect consistent with cables; fix fluid connectivity.
- Non-Goals: change the protocol; change SimulationCore; mask machine→pipe edges.

## Decisions
- **Where**: implement entirely in `pipe_network` (PipeNetworkService), reusing the existing
  synchronous `pipe_meta_` cache. No async `GetBlock` refactor — the cache already holds pipe
  meta from block-changed events.
- **Face-bit convention**: meta bits `{+X,-X,+Y,-Y,+Z,-Z}` = indices 0..5; meta 0 ⇒ 0x3F.
  Introduce one canonical offset table indexed by meta bit
  (`bit0=+X→(1,0,0)`, `bit1=-X→(-1,0,0)`, `bit2=+Y→(0,1,0)`, `bit3=-Y→(0,-1,0)`,
  `bit4=+Z→(0,0,1)`, `bit5=-Z→(0,0,-1)`) and a `metaBitSet(meta,f)` helper (`meta==0 ⇒ 0x3F`).
  NOTE: the existing item fallback scan (`PipeNetworkService.cpp:660-662`) uses a *different*
  face ordering `{-Y,+Y,-Z,+Z,-X,+X}`; replace it with the canonical table to avoid the
  remap foot-gun.
- **Edge rules**: an edge across face `f` is created only if
  `metaBitSet(fromMeta,f) && metaBitSet(toMeta, f^1)`. Machine→pipe/cable stays explicit.
- **Pipe↔pipe**: currently no item/fluid pipe↔pipe edges are created; add them on node
  registration / `rebuildItemNetworks` using the same rule (this also makes pure pipe runs
  conductive, which they currently are not).

## Risks / Trade-offs
- Changing the item fallback face ordering could shift any (currently implicit) orientation
  assumptions — mitigated by using the canonical meta-bit table everywhere.
- Adding pipe↔pipe edges expands the item/fluid graph; matches existing cable behavior.

## Migration Plan
Backward compatible: meta 0 ⇒ all faces connected, so pre-mask placements keep working.
Rollback = revert the service edits; no schema/data migration.

## Open Questions
- None blocking. Confirm with user that the in-service (not SimulationCore) approach is
  acceptable given the stale design doc.
