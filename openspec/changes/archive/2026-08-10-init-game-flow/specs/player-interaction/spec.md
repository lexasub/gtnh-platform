# player-interaction Specification

## Purpose

Define the player interaction and inventory pipeline — CAS block placement, crafting, drag-and-drop, inventory persistence, machine windows, world exploration, and game scenarios.

## ADDED Requirements

### Requirement: Server-Authoritative Game Scenario Execution

The system SHALL execute game scenarios on the server (SimulationCore), not on the client.
Scenario contents SHALL be a data table owned by the server: index, name, target mode, items to
grant (packed item ids), a clear-first flag, and a quest book era. On `StartScenarioReq`
(`scenario_index`), the server SHALL validate the request, then apply the scenario to the player:
optionally replace the inventory via `PlayerInventoryStore::setSlots`, grant items via `giveItem`,
set the game mode via `setGameMode`, and respond with `StartScenarioResp`. The resulting inventory
SHALL reach the client through the existing authoritative `player.inventory.update` push
(`postMutation` → `kInventoryUpdate`), published before the response.

#### Scenario: Start scenario 0 grants the starter set

- **GIVEN** a connected player with an empty inventory
- **WHEN** SimulationCore receives `StartScenarioReq` with `scenario_index = 0`
- **THEN** the server SHALL clear the player's 40-slot inventory via `setSlots`
- **AND** SHALL grant a crafting table (packed `0:10:11:1` = 22529) and a wooden pickaxe (packed `0:11110:3` = 30723) via `giveItem`
- **AND** SHALL set the player's game mode to SURVIVAL via `setGameMode`
- **AND** SHALL publish the full inventory snapshot on `player.inventory.update` before publishing `StartScenarioResp(success = true, game_mode = SURVIVAL, quest_book_era = VAGRANT)`

#### Scenario: Client receives the authoritative snapshot

- **GIVEN** the server executed a scenario
- **WHEN** `PlayerInventoryStore` fires `postMutation`
- **THEN** the full snapshot SHALL be published on `player.inventory.update` and relayed to the client as `kInventoryUpdate` (6)
- **AND** the client SHALL NOT mutate its inventory slots locally for the scenario — the pushed snapshot is the source of truth

#### Scenario: Invalid scenario request rejected

- **GIVEN** a client sends a scenario index outside the server's table, or `player_id = 0`
- **WHEN** the scenario handler validates the request
- **THEN** the server SHALL respond `StartScenarioResp(success = false)` with an error message
- **AND** SHALL NOT change inventory or game mode

### Requirement: Inventory Persistence for Scenario Mutations

Inventory changes produced by scenario execution SHALL be persisted through the existing
per-mutation path.

#### Scenario: Scenario grants persisted to MetaDB

- **GIVEN** the scenario handler calls `setSlots` / `giveItem`
- **WHEN** the `onChange` callback fires
- **THEN** a `SetInventorySlotReq` SHALL be published on `meta_db.inventory.set`
- **AND** MetaDB SHALL upsert each slot, so a player reconnecting after a scenario keeps the granted inventory
