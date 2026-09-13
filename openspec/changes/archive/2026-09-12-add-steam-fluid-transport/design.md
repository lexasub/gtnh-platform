# Design: Steam Fluid Transport

## Context

`update-pipe-fluid-face-mask` (completed) added the masked `connectNodeNeighbors`
scan to `handleFluidNodeUpdate`, so fluid pipes now attach to *fluid* machine nodes
(`fluidCapacity > 0`). But the two steam producers (heat boiler `1110:01:1`, solid
boiler `1110:01:0`) and all steam consumers (`steam_macerator` et al.) are **energy
machines**: they register via `handleNodeUpdate` → `addNodeWithId(id, ..., 1)` →
`default:` → `fluidCapacity = 0` → no pipe edges. Steam consumers additionally have
no energy request path at all (no `EnergyType::STEAM` branch in `MachineSystem`).

The fluid transport layer (BFS consume, `FluidFlowEvent`, `FluidFlowHandler`
Case 1 FluidStorage / Case 2 STEAM-energy, `fluid.consume.response` subscription)
is complete and waiting for machines to use it.

## Goals / Non-Goals

- Goals: boilers become steam fluid sources; steam machines consume steam through
  fluid pipes and actually run; pipe↔boiler edges form.
- Non-Goals: water input (no water source exists; user decision), `FluidStorage`
  tanks, yaml-driven fluid profiles, client flange cosmetics, fluid-type fidelity.

## Decisions

- **One node per machine.** `machine_nodes_` is keyed by position and
  `protocol_to_mgr_` by node id; a machine's energy and fluid registrations share
  the same node id (the ECS entity id) and therefore the same `PipeNode` +
  `NodeState`. The boiler publishes both `energy.node.update` and
  `fluid.node.update` with the same amount/is_source — the shared `NodeState.energy`
  becomes the steam pool. Consistency requirement: both publish streams must carry
  the same amount, or the client state and the fluid pool diverge.
- **Fluid-capacity upgrade in `handleFluidNodeUpdate`.** When the node already
  exists with `fluidCapacity == 0` (registered as an energy machine with
  blockId=1), call `setNodeFluid(mgr_id, amount, capacity, fluid_id, is_source,
  is_sink)` before the `connectNodeNeighbors` scan. The scan is already called
  unconditionally at the end of `handleFluidNodeUpdate`, so edges form on the next
  fluid update — no extra rebuild needed.
- **Steam consumer request flow.** `MachineSystem` STEAM branch:
  publish fluid node (registers capacity + edges) → `sendFluidRequest(node_id, pos,
  fluid_id=steam, needed)` → PipeNetwork BFS drains the boiler source →
  `FluidFlowEvent` to the boiler (drains its steam pool) + `FluidConsumeResp` to
  the consumer → `SimCoreMessageHandler` routes to
  `MachineSystem::onFluidConsumeResponse`, crediting `energy.current` from the
  oldest pending fluid consume (same pattern as the energy channel —
  `FluidConsumeResp` carries no node id).
- **Boiler steam pool.** Heat boiler stores steam in `SteamOutputComponent` (not a
  STEAM `EnergyStorage`) → `FluidFlowHandler` gets a new Case 2b that drains
  `steam_stored` and re-publishes the fluid node state. Solid boiler keeps steam in
  a STEAM `EnergyStorage` → existing Case 2 covers it unchanged.
- **Registration order independence.** Energy-first or fluid-first both converge:
  `handleNodeUpdate` creates the node (0 fluid caps) and `handleFluidNodeUpdate`
  upgrades it. If fluid publishes first, the node is created with
  `BLOCK_ID_FLUID_PIPE` semantics and the energy update only refreshes `NodeState`.

## Risks / Trade-offs

- Shared `NodeState.energy` between the energy and fluid channels: the two publish
  streams must stay consistent (same amount, same source flag). Mitigation: the
  boilers publish both updates in the same tick from the same pool.
- `FluidConsumeResp` has no node id → oldest-pending lookup (inherited from the
  energy channel; already the existing behavior for HEAT/ELECTRICITY/ROTATION).
- Fluid-type fidelity is pre-existing and out of scope: `handleFluidConsumeRequest`
  drains any `is_source` node without checking `req->fluid_id()`.

## Migration Plan

Backward compatible: machines that never publish `fluid.node.update` keep their
current behavior (energy-only, no fluid edges). Rollback = revert the service edits;
no schema/data migration.
