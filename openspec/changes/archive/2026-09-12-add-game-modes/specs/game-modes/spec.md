# game-modes Specification

## ADDED Requirements

### Requirement: Game Mode Definition
The system SHALL define a `GameMode` enum (SURVIVAL=0, CREATIVE=1, ADVENTURE=2,
SPECTATOR=3) with a `GameModeName()` helper, and SHALL map each mode to capabilities
via a permission matrix (`GameModePerm` in `Common/Inventory.h`): canFly, noclip,
canBreak, canPlace, infiniteItems.

| Mode | canFly | noclip | canBreak | canPlace | infiniteItems |
|------|--------|--------|----------|----------|---------------|
| SPECTATOR | true | true | false | false | true |
| CREATIVE | true | true | true | true | true |
| SURVIVAL | false | false | true | true | false |
| ADVENTURE | false | false | false | false | false |

`canBreak`/`canPlace` and `infiniteItems` feed the block-interaction and NEI gates;
the exact per-mode block-interaction semantics (including inventory consumption) are
defined by the separate `add-interaction-mode-gating` change.

#### Scenario: Default mode is Creative
- **WHEN** the client starts
- **THEN** `InventoryState::gameMode` SHALL be `CREATIVE`
- **AND** movement and interaction SHALL behave as today (fly, noclip, break/place enabled)

#### Scenario: Survival restricts capabilities
- **GIVEN** `InventoryState::gameMode` is `SURVIVAL`
- **THEN** `GameModePerm::CanFly(SURVIVAL)` SHALL be `false`
- **AND** `GameModePerm::InfiniteItems(SURVIVAL)` SHALL be `false`
- **AND** the client SHALL NOT spawn items from NEI

### Requirement: Mode State and Console Switching
The client SHALL own the current mode in `InventoryState::gameMode`
(client-authoritative during the dev phase; server trusts the client) and SHALL switch
it via the existing `/gamemode` console command.

#### Scenario: Switch mode via console
- **GIVEN** the player enters `/gamemode 1` in the console
- **WHEN** `ConsoleWindow` parses the argument as a valid `GameMode` value (0-3)
- **THEN** `InventoryState::gameMode` SHALL be updated immediately
- **AND** the client SHALL send `GameModeChange` to the server

#### Scenario: Unknown mode rejected
- **GIVEN** the player enters `/gamemode 7` in the console
- **WHEN** `ConsoleWindow` validates the argument
- **THEN** the mode SHALL remain unchanged
- **AND** the console SHALL report an error listing valid modes (0-3)

### Requirement: Mode-Aware Movement
The client SHALL move according to the current mode: SPECTATOR and CREATIVE use fly +
noclip (camera free-fly); SURVIVAL and ADVENTURE use gravity, walking, jumping, and
AABB collision against solid blocks. Movement state (position, velocity, onGround)
SHALL live in a `PlayerController` (`game_client/Player/PlayerController.cpp`) that the
camera renders from.

#### Scenario: Spectator flies through blocks
- **GIVEN** the current mode is SPECTATOR or CREATIVE
- **WHEN** the player moves
- **THEN** the camera SHALL move freely in any direction with no collision
- **AND** SHALL pass through solid blocks

#### Scenario: Survival falls and lands
- **GIVEN** the current mode is SURVIVAL and the player is airborne above a solid block
- **WHEN** gravity is applied
- **THEN** the player SHALL accelerate downward until colliding with the solid block
- **AND** SHALL stop at the block surface (standing on it)

#### Scenario: Survival cannot walk through walls
- **GIVEN** the current mode is SURVIVAL and a solid block is in front of the player
- **WHEN** the player walks forward
- **THEN** the player SHALL stop at the block boundary and SHALL NOT intersect it

### Requirement: NEI Item Spawning Gating
Item spawning from the NEI panel SHALL be allowed in SPECTATOR and CREATIVE and SHALL
be blocked in SURVIVAL and ADVENTURE (`GameModePerm::InfiniteItems`).

#### Scenario: NEI spawn in spectator
- **GIVEN** the current mode is SPECTATOR or CREATIVE and the NEI panel is open
- **WHEN** the player clicks an item
- **THEN** the item SHALL be spawned into the selected hotbar slot (current behavior)

#### Scenario: NEI spawn blocked in survival and adventure
- **GIVEN** the current mode is SURVIVAL or ADVENTURE and the NEI panel is open
- **WHEN** the player clicks an item
- **THEN** no item SHALL be spawned
- **AND** the panel SHALL indicate spawning is disabled
