## ADDED Requirements

### Requirement: Container-Click Inventory Action
`Protocol::InventoryAction` SHALL carry a container-click descriptor — `player_id`, `action_type` (CLICK / QUICK_MOVE / DROP / DRAG_PLACE / PICKUP_ALL), `button` (LMB/RMB), `mods` (shift/ctrl), `container_id`, `slot` and `count` — so the server computes the full operation against its authoritative state instead of trusting pre-computed source/target slots.

#### Scenario: Click sent on a slot
- **GIVEN** the player clicks a slot inside the player inventory or an open container window
- **WHEN** the client sends an `InventoryAction` with the click descriptor (`container_id`, `slot`, `button`, `mods`, `action_type`)
- **THEN** the Gateway SHALL forward it to the `player.inventory.actions` topic unchanged
- **AND** the server SHALL NOT trust any pre-computed slot contents from the client

#### Scenario: Quick-move is a first-class action
- **GIVEN** the player shift/ctrl-clicks a slot
- **WHEN** the client sends `action_type=QUICK_MOVE`
- **THEN** the server SHALL move the stack to the complementary inventory (hotbar↔main in a player window; player↔container in a container window) with top-down stacking

#### Scenario: Drop outside
- **GIVEN** the player presses Q or clicks outside the window while holding the cursor stack
- **WHEN** the client sends `action_type=DROP`
- **THEN** the server SHALL discard the cursor stack or the hovered slot's stack as specified

### Requirement: Machine Window Open/Close
The Gateway SHALL route machine-window open/close requests to SimulationCore as container sessions: `kMachineOpenReq=18` and `kMachineCloseReq=46` (reusing `Protocol::ContainerOpenReq` — no new table), published as `player.machine.open` / `player.machine.close`. The open acknowledgement rides the existing `player.inventory.update` → `kInventoryUpdate` relay.

#### Scenario: Machine window opens
- **GIVEN** the player interacts with a machine block
- **WHEN** the client sends `ContainerOpenReq` with `kMachineOpenReq=18`
- **THEN** the Gateway SHALL publish `player.machine.open` to SimulationCore unchanged
- **AND** SimulationCore SHALL restore the machine's saved slots from EntityStateStore, register a `container_id=1` session and acknowledge via a full `player.inventory.update` snapshot

#### Scenario: Machine window closes
- **GIVEN** the player closes an open machine window
- **WHEN** the client sends `ContainerOpenReq` with `kMachineCloseReq=46`
- **THEN** the Gateway SHALL publish `player.machine.close` to SimulationCore unchanged
- **AND** SimulationCore SHALL persist the session's live slots (blob keyed by `machine_id`) and deregister the per-player session

### Requirement: Server-Authoritative Inventory Snapshot
`Protocol::InventoryUpdate` SHALL carry the server-owned cursor stack and the open-container slots (`cursor`, `container_id`, `container_pos`, `container_slots`) in addition to the 40 player slots, so a single snapshot drives the player inventory, the cursor and the open container window.

#### Scenario: Snapshot after every mutation
- **GIVEN** the server applied any inventory or container operation
- **WHEN** the server publishes `player.inventory.update`
- **THEN** the payload SHALL include player slots, the cursor stack and the open container slots
- **AND** the client SHALL replace its local inventory view wholesale (no client-side authoritative mutation)

#### Scenario: Cursor is rendered from the snapshot
- **GIVEN** the player is holding a stack on the cursor
- **WHEN** the client receives an `InventoryUpdate`
- **THEN** it SHALL render the held stack from the `cursor` field and track it as the drag preview
