## ADDED Requirements

### Requirement: CAS Block Placement
The system SHALL use Compare-And-Swap for block placement to prevent race conditions.

#### Scenario: Optimistic placement succeeds
- **GIVEN** a player places a block at position (x,y,z)
- **WHEN** the SetBlockCAS finds expected_block_id matches current
- **THEN** the block is placed and BlockAck(ACCEPTED) is sent to client

#### Scenario: CAS conflict triggers revert
- **GIVEN** a player places a block but another player changed it first
- **WHEN** the SetBlockCAS finds expected_block_id does NOT match
- **THEN** BlockAck(CONFLICT, actual_id) is sent and client reverts the block

### Requirement: Crafting Pipeline
The system SHALL process crafting requests through RecipeManager.

#### Scenario: Valid recipe found
- **GIVEN** a player submits a CraftRequest with grid items
- **WHEN** RecipeManager finds a matching recipe
- **THEN** ingredients are consumed from inventory and result is added
- **AND** CraftResponse(success=true, result) is returned

#### Scenario: No recipe matches
- **GIVEN** a player submits a CraftRequest
- **WHEN** RecipeManager finds no matching recipe
- **THEN** CraftResponse(success=false) is returned

### Requirement: Inventory Persistence
The system SHALL persist player inventory across sessions via MetaDB.

#### Scenario: Inventory saved on logout
- **GIVEN** a player disconnects
- **WHEN** the disconnect handler runs
- **THEN** inventory slots are saved to MetaDB SQLite

#### Scenario: Inventory loaded on login
- **GIVEN** a player connects
- **WHEN** the welcome message is sent
- **THEN** inventory is loaded from MetaDB and sent to client

### Requirement: Machine Window UI
The system SHALL display machine state in a data-driven UI window.

#### Scenario: Machine window shows progress
- **GIVEN** a player opens a machine block
- **WHEN** the MachineWindow renders
- **THEN** it shows energy bar, recipe progress, input/output slots
