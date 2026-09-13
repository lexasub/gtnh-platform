# Change: Per-Face Connection Masking for Item/Fluid Pipes

## Why
The wrench already toggles individual pipe/cable faces (block meta byte, bits 0-5 =
`{+X,-X,+Y,-Y,+Z,-Z}`, meta 0 = all six faces connected) and `CableGraph` honors this
mask for energy cables. Item pipes and fluid pipes do not:

- Item machine→pipe edges are built by an **unconditional** synchronous neighbor scan that
  **ignores the mask**.
- Fluid pipe connectivity is **not built at all** (the fluid handler has no neighbor scan).
- Pipe↔pipe edges for item/fluid are **never created**.

A wrench-disconnected face therefore still conducts item flow (and fluid doesn't conduct
at all). This makes per-face disconnect inconsistent across transport types and breaks the
GTNH-style "disconnect this face" UX for item/fluid.

## What Changes
Target is **`PipeNetworkService` (pipe_network service)** — that is where item/fluid
connectivity is actually computed. (Note: the ECS `connected_nodes` list carried by
`ItemClient`/`FluidClient::publishNodeUpdate` is always empty in the current code, so the
adjacency is built in-service, NOT in SimulationCore. No SimulationCore change is required.)

- Extend the existing synchronous `pipe_meta_` cache (`PipeNetworkService.h:56`, populated
  today only for cables in `handleBlockChanged`) to cover all `isPipeBlock` blocks.
- Apply the mask in the item machine→pipe fallback scan (`handleItemNodeUpdate`):
  skip `addEdge` unless both source and neighbor have the facing bits set
  (`meta==0 ⇒ 0x3F`; require `fromMeta&(1<<f)` and `toMeta&(1<<(f^1))`).
- Build **pipe↔pipe** edges for item/fluid (currently missing) on node registration / rebuild,
  using the same mask rule.
- Add the equivalent masked fallback scan to `handleFluidNodeUpdate` (fluid currently builds
  no connectivity).
- Add a `uint8_t meta` field to `PipeNode` (`PipeNetwork.h`), mirroring `CableNode`.
- Machine→pipe/cable edges stay explicit (mask not applied), matching the existing cable rule.
- No protocol changes, no SimulationCore changes.

## Impact
- Affected specs: `pipes-cables-transport`
- Affected code:
  - `src/services/pipe_network/PipeNetworkService.cpp` — `handleBlockChanged` (meta cache),
    `handleItemNodeUpdate` (masked fallback scan), `handleFluidNodeUpdate` (add masked
    fallback scan), node-registration / rebuild pipe↔pipe edge creation
  - `src/services/pipe_network/PipeNetwork.h` — `PipeNode.meta`
  - `src/services/pipe_network/PipeNetwork.cpp` — `addNode` / `rebuildItemNetworks`
  - `src/services/pipe_network/PipeNetworkService.h` — `pipe_meta_` (extend population)
- Out of scope: cables (already mask-aware in `CableGraph.cpp`), SimulationCore
  `connected_nodes` (unused for item/fluid).
