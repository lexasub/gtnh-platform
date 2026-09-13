# protocol Specification

## ADDED Requirements

### Requirement: Game Mode Messages
The protocol SHALL define a single `GameModeChange` table (in `core.fbs`) carrying
`player_id` and `new_mode` (a `GameMode` enum mirroring the client values:
SURVIVAL=0, CREATIVE=1, ADVENTURE=2, SPECTATOR=3). The client-gateway wire constant
SHALL be `kGameModeChange = 30` (defined in C++ `GatewayMsg` and `NetClient.h`).

#### Scenario: Client requests a mode change
- **GIVEN** the client switches mode via `/gamemode`
- **WHEN** it sends `GameModeChange` (wire 30) carrying `player_id` and `new_mode`
- **THEN** gateway SHALL relay it to SimulationCore on the game-mode topic
  (`player.gamemode.change`)

#### Scenario: Server broadcasts the mode change
- **GIVEN** SimulationCore stored the player's new mode
- **WHEN** it publishes the change back to the client
- **THEN** gateway SHALL send `GameModeChange` to the affected client
- **AND** the client SHALL NOT re-apply the mode, because it already applied it
  locally on the request (server trusts the client during the dev phase)

#### Scenario: Server trusts client during dev phase
- **GIVEN** a client sends any `GameModeChange`
- **WHEN** SimulationCore processes it
- **THEN** it SHALL accept it without permission checks (validation deferred to beta,
  when the same message handler gains a permission gate)
