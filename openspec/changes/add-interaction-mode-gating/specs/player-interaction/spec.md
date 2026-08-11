# player-interaction Specification

## ADDED Requirements

### Requirement: Mode-Gated Block Interaction
Block break and place SHALL be gated by the game mode via the `GameModePerm` matrix:
`CanBreak`/`CanPlace` are true for CREATIVE and SURVIVAL and false for ADVENTURE and
SPECTATOR. The client interaction path SHALL use these predicates (not a separate
inline mode list).

| Mode | CanBreak | CanPlace | Placement cost |
|------|----------|----------|----------------|
| CREATIVE | true | true | none (infinite items) |
| SURVIVAL | true | true | server-side consumption |
| ADVENTURE | false | false | — |
| SPECTATOR | false | false | — |

#### Scenario: Creative breaks and places freely
- **GIVEN** the current mode is CREATIVE
- **WHEN** the player left-clicks a block or right-clicks a placement position
- **THEN** the action SHALL be sent
- **AND** no inventory slot SHALL be consumed (server applies the placement without a
  cost)

#### Scenario: Survival places with server-side consumption
- **GIVEN** the current mode is SURVIVAL and the selected slot holds a block
- **WHEN** the player right-clicks a placement position
- **THEN** the place action SHALL be sent
- **AND** SimulationCore SHALL deduct the block from the player inventory
      (server-authoritative; the client does not locally decrement)
- **AND** the resulting inventory SHALL be reflected back via `InventoryUpdate`

#### Scenario: Adventure and spectator cannot interact
- **GIVEN** the current mode is ADVENTURE or SPECTATOR
- **WHEN** the player left-clicks or right-clicks a block
- **THEN** no break or place action SHALL be sent
