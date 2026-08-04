## ADDED Requirements

### Requirement: Rewards Integrated with Inventory
The system SHALL add quest reward items to the player inventory upon redemption.

#### Scenario: Reward granted to inventory
- **GIVEN** a quest is completed with a non-zero reward
- **WHEN** the reward event is processed and redemption runs
- **THEN** the reward items SHALL be added to the player inventory via MetaDB inventory API
- **AND** the reward row SHALL be marked redeemed (redeemed=1)

#### Scenario: Redemption is idempotent
- **GIVEN** a reward row already marked redeemed
- **WHEN** a redemption request arrives again
- **THEN** no duplicate items SHALL be granted
