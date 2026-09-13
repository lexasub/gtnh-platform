# inventory-interaction Specification

## Purpose
Server-authoritative inventory interaction: drag-and-drop, slot actions, and grid state.
## Requirements
### Requirement: Inventory Drag-and-Drop
The system SHALL provide a drag-and-drop inventory state machine in the client (DragManager) with server-side application via `InventoryAction`.

#### Scenario: Pickup transitions to Holding
- **GIVEN** DragManager is in Idle state
- **WHEN** the player left-clicks a non-empty slot
- **THEN** DragManager SHALL pick up the entire stack and transition to Holding
- **AND** right-click SHALL pick up `ceil(count/2)` and send `kActionSplit` (1)
- **AND** shift-click SHALL quick-move the entire stack and send `kActionQuickMove` (3)

#### Scenario: Place, merge, and swap while holding
- **GIVEN** DragManager is in Holding state
- **WHEN** the player left-clicks an empty slot
- **THEN** the held stack SHALL be placed there and `kActionMove` (0) SHALL be sent
- **AND** left-click on a same-item non-full slot SHALL merge stacks (up to 64) and send `kActionMove` (0)
- **AND** left-click on a different-item slot SHALL swap the held and target stacks and send `kActionMove` (0)

#### Scenario: Drop and cancel
- **GIVEN** the player is holding an item
- **WHEN** the player presses Q
- **THEN** the held item SHALL be dropped/destroyed and `kActionDrop` (2) SHALL be sent
- **AND** pressing ESC SHALL return the item to its source slot without a network action
- **AND** Q while hovering a slot (not dragging) SHALL drop that slot's item

#### Scenario: Server applies inventory action
- **GIVEN** the client sends `InventoryAction` (kInventoryAction=7) with `action_type` (0=MOVE, 1=SPLIT, 2=DROP), `source_slot`, `target_slot`, `count`
- **WHEN** Gateway relays it on topic `player.inventory.actions` and `InventoryActionHandler` runs
- **THEN** the action SHALL be applied to the player's 40-slot inventory in `PlayerInventoryStore`
- **AND** the resulting `InventoryUpdate` SHALL be published on `player.inventory.update` and relayed to the client as `kInventoryUpdate` (6)

### Requirement: Inventory Persistence
The system SHALL persist player inventory in MetaDB SQLite per mutation and load it on login.

#### Scenario: Inventory saved on every mutation
- **GIVEN** SimulationCore mutates a player's inventory via `setSlots` or `giveItem`
- **WHEN** the `onChange` callback fires
- **THEN** a `SetInventorySlotReq` SHALL be published on topic `meta_db.inventory.set`
- **AND** MetaDB SHALL upsert the slot into the `inventory` table (player_id, slot, block_id, count)
- **AND** the `onChange` callback SHALL NOT run for the same mutation twice

#### Scenario: Inventory loaded on login
- **GIVEN** a player connects and Gateway publishes `player.joined`
- **WHEN** MetaDB's `handlePlayerJoined` runs
- **THEN** it SHALL read the player's inventory from SQLite and publish it as an `InventoryUpdate` on topic `player.inventory.load`
- **AND** SimulationCore's `InventoryLoadHandler` SHALL apply it to `PlayerInventoryStore` via `applyUpdate`
- **AND** the inventory SHALL be re-published on `player.inventory.update` so the client receives it

#### Scenario: No explicit save on logout
- **GIVEN** a player disconnects and Gateway publishes `player.left`
- **WHEN** MetaDB's `handlePlayerLeft` runs
- **THEN** it SHALL save only the player position
- **AND** SHALL NOT re-save inventory, because inventory is already persisted per mutation

### Requirement: Inventory Persistence for Scenario Mutations

Inventory changes produced by scenario execution SHALL be persisted through the existing
per-mutation path.

#### Scenario: Scenario grants persisted to MetaDB

- **GIVEN** the scenario handler calls `setSlots` / `giveItem`
- **WHEN** the `onChange` callback fires
- **THEN** a `SetInventorySlotReq` SHALL be published on `meta_db.inventory.set`
- **AND** MetaDB SHALL upsert each slot, so a player reconnecting after a scenario keeps the granted inventory

