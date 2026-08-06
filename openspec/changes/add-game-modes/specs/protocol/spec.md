# protocol Specification

## Purpose
TBD - created by archiving change add-game-modes. Update Purpose after archive.

## ADDED Requirements

### Requirement: Game Mode Messages
The client-gateway protocol SHALL define two game mode messages: `SetGameModeReq`
(client→gateway) and `GameModeChanged` (gateway→client). The `GameMode` values in the
protocol SHALL mirror the client enum: SURVIVAL=0, CREATIVE=1, ADVENTURE=2,
SPECTATOR=3.

#### Scenario: Client requests a mode change
- **GIVEN** the client switches mode via `/gamemode`
- **WHEN** it sends `SetGameModeReq` (GatewayPayload union index 27) carrying
  `player_id` and `mode`
- **THEN** gateway SHALL relay it to SimulationCore on the game-mode topic

#### Scenario: Server broadcasts the mode change
- **GIVEN** SimulationCore stored the player's new mode
- **WHEN** it publishes the change back to the client
- **THEN** gateway SHALL send `GameModeChanged` (GatewayPayload union index 28)
  carrying `player_id` and `mode` to the affected client
- **AND** the client SHALL NOT re-apply the mode, because it already applied it
  locally on the request (server trusts the client during the dev phase)

#### Scenario: Server trusts client during dev phase
- **GIVEN** a client sends any `SetGameModeReq`
- **WHEN** SimulationCore processes it
- **THEN** it SHALL accept it without permission checks (validation deferred to beta,
  when the same message handler gains a permission gate)
