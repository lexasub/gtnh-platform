# Inventory Actions Protocol — Analysis Task

## Objective
Analyze current `InventoryAction` (core.fbs) and propose migration to first-class action types (MoveItems, DropItems, CraftItems) with server-side permission callbacks.

## Current State
Single `InventoryAction` table with `action_type:uint8`. GameClient sends raw actions, SimulationCore has `InventoryActionHandler` that processes them.

## Deliverables
1. Migration proposal: new FlatBuffers tables + union
2. Changes to Gateway (new action relay rules)
3. Changes to SimulationCore InventoryActionHandler (dispatch by union type)
4. Changes to GameClient (action construction)
5. Backward compatibility plan

## Constraints
- Must not break existing GameClient connections during transition
- Server-side callbacks (`allowPut`/`allowTake`) are optional — if absent, default = allow
