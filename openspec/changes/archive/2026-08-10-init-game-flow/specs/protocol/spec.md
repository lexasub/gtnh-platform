# protocol Specification

## Purpose

Define the communication protocol for GTNH Platform — wire format, message types, topic conventions, and delivery guarantees for all service-to-service and client-to-gateway communication.

## ADDED Requirements

### Requirement: Game Scenario Messages

The client-gateway protocol SHALL define two game scenario messages: `StartScenarioReq`
(client→gateway) and `StartScenarioResp` (gateway→client). `StartScenarioReq` SHALL carry
`player_id` and `scenario_index` (uint8). `StartScenarioResp` SHALL carry `player_id`,
`scenario_index`, `success` (bool), `error` (string), `game_mode` (`Protocol::GameMode`), and
`quest_book_era` (uint8). The messages SHALL use wire indices `kStartScenarioReq = 31` and
`kStartScenarioResp = 32` (next free after `kGameModeChange = 30`); the C++ `GatewayMsg` constants
in `gateway.h` / `NetClient.h` SHALL govern the wire, and the fbs `GatewayPayload` union SHALL be
kept consistent with them (the union indices are documented as known-stale).

#### Scenario: Client requests the initial scenario

- **GIVEN** the player runs `/startGameScenario 0` in the console
- **WHEN** the client sends `StartScenarioReq` (`kStartScenarioReq = 31`) carrying `player_id` and `scenario_index = 0`
- **THEN** gateway SHALL relay it to SimulationCore on the `player.scenario.start` topic

#### Scenario: Server confirms the scenario result

- **GIVEN** SimulationCore executed the scenario for a player
- **WHEN** it publishes the result on `player.scenario.start.response`
- **THEN** gateway SHALL send `StartScenarioResp` (`kStartScenarioResp = 32`) to the client, carrying `success`, `game_mode`, and `quest_book_era`
- **AND** on failure it SHALL carry `success = false` with a human-readable `error` and no game-mode change

#### Scenario: Request rejected for invalid input

- **GIVEN** a client sends `StartScenarioReq` with `player_id = 0` or a `scenario_index` outside the server's scenario table
- **WHEN** SimulationCore validates the request
- **THEN** it SHALL respond with `StartScenarioResp(success = false)` and an error message
- **AND** SHALL NOT mutate inventory or game mode
