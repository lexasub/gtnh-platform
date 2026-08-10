## ADDED Requirements

### Requirement: Machine Window Shows Live Container Slots
An open machine window SHALL render as `container_id=1` backed by the machine's live ECS inventory, so slot mutations from simulation (item flow, machine crafting) are visible without re-opening the window.

#### Scenario: Machine container is live
- **GIVEN** a machine window is open as `container_id=1`
- **WHEN** SimulationCore writes an item into the machine's ECS `InventoryContainer` (item flow, recipe output)
- **THEN** the client window SHALL reflect the updated slots (via `player.inventory.update` snapshot or per-slot update) without re-opening

#### Scenario: Window close persists
- **GIVEN** the player closes the machine window
- **WHEN** `kMachineCloseReq=46` reaches SimulationCore
- **THEN** the session's live slots SHALL be persisted to EntityStateStore keyed by `machine_id`
- **AND** the per-player session SHALL be deregistered

### Requirement: Server-Authoritative Inventory Manipulation
The system SHALL implement Minecraft-style item manipulation as server-authoritative click semantics covering the player inventory and open containers (chest, machine, workbench grid): pick-up, place, merge, swap, half-split, place-one, drag-distribute, quick-move, double-click pick-up-all and drop.

#### Scenario: Left-click pick-up and place
- **GIVEN** the cursor is empty and the player left-clicks a non-empty slot
- **WHEN** the server applies the CLICK rule
- **THEN** the whole stack SHALL move to the cursor and the slot SHALL empty
- **AND** clicking an empty slot with a non-empty cursor SHALL place the whole stack, a same-item non-full slot SHALL merge up to the 64 stack limit (remainder stays on the cursor), and a different-item slot SHALL swap cursor and slot

#### Scenario: Right-click takes half
- **GIVEN** the cursor is empty and the player right-clicks a non-empty slot
- **WHEN** the server applies the CLICK rule with `button=RMB`
- **THEN** ceil(count/2) SHALL move to the cursor and the remainder SHALL stay in the slot
- **AND** with a non-empty cursor on an empty or same-item non-full slot SHALL place exactly 1 item

#### Scenario: Right-drag distributes one per slot
- **GIVEN** the player holds a stack on the cursor and drags with RMB across slots
- **WHEN** each visited slot emits `action_type=DRAG_PLACE`
- **THEN** the server SHALL place exactly 1 item per visited empty or same-item non-full slot, decrementing the cursor, and SHALL stop when the cursor empties

#### Scenario: Shift-click moves between inventories
- **GIVEN** a container window is open and the player shift-clicks a slot
- **WHEN** the server applies QUICK_MOVE
- **THEN** an item in a container slot SHALL move to the player inventory and an item in a player slot SHALL move to the container, stacking into existing stacks first and filling empty slots in order
- **AND** in a player-only window the move SHALL be between the hotbar (0–9) and the main grid

#### Scenario: Double-click collects a stack
- **GIVEN** the cursor holds an item and the player double-clicks another slot with the same item
- **WHEN** the server applies PICKUP_ALL
- **THEN** all matching stacks in the open window SHALL merge onto the cursor up to the 64 stack limit

#### Scenario: Drop and cancel
- **GIVEN** the player presses Q or clicks outside the window
- **WHEN** the server applies DROP
- **THEN** the cursor stack SHALL be discarded, or the hovered slot's stack when the cursor is empty
- **AND** pressing ESC while holding SHALL return the cursor stack to its origin slot

#### Scenario: Operations run on authoritative state
- **GIVEN** any of the above interactions
- **WHEN** the server computes the result
- **THEN** the computation SHALL run against the server's player slots, cursor and container slots, and SHALL publish a full `InventoryUpdate` snapshot after any mutation
- **AND** the client SHALL NOT mutate authoritative slots locally
