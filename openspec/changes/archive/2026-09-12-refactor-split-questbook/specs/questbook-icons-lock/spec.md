## ADDED Requirements

### Requirement: Quest requirement icons
The quest book detail view SHALL render the items/blocks a quest requires as item icons sourced from the item registry, not as bare text.

#### Scenario: Obtain objective icon
- **GIVEN** a quest with a requirement of kind `obtain` is selected in the quest book detail view
- **THEN** the requirement SHALL render as an item icon for its `item` using `renderlib::TextureAtlas::GetItemUV`
- **AND** the icon SHALL be accompanied by the required quantity and the player's current held quantity

#### Scenario: Craft / place / machine requirement icon
- **GIVEN** a quest with a requirement of kind `craft`, `place`, or `machine` is selected
- **THEN** the target item SHALL render as an item icon
- **AND** the icon SHALL be accompanied by the item name, required quantity, and the requirement kind
- **AND** for kind `machine` the machine item SHALL render as an additional icon

#### Scenario: Consumed requirement badge
- **GIVEN** a requirement with `consume: true` renders
- **THEN** the requirement SHALL show a "taken on completion" indicator
- **AND** a requirement with `consume: false` SHALL show a "kept" indicator

#### Scenario: EXCHANGE cost icon
- **GIVEN** an EXCHANGE quest is selected
- **THEN** the cost item (`costItemId`) SHALL render as an item icon with its count
- **AND** the reward SHALL render as an item icon with its count

#### Scenario: Icons resolved from item registry
- **GIVEN** any quest requirement or prize item id
- **WHEN** the icon is rendered
- **THEN** the UV rectangle SHALL be resolved via `renderlib::TextureAtlas::GetItemUV` (item_icons.csv → block_faces.csv → default fallback)
- **AND** the display name SHALL be resolved via `ItemRegistry::GetName`
- **AND** NO hardcoded placeholder texture or color square SHALL be substituted when the item exists in the registry
### Requirement: Quest prize icons
The quest book SHALL render each quest's prize(s) as item icons with quantity and name, sourced from `quest_rewards.json`.

#### Scenario: Reward icon shown for non-exchange quests
- **GIVEN** a non-exchange quest with one or more rewards defined in `quest_rewards.json` is selected
- **THEN** each reward SHALL render as an item icon using `TextureAtlas::GetItemUV`
- **AND** the icon SHALL show the reward count and item name

#### Scenario: Choice rewards render all options
- **GIVEN** a quest with `choice_of` rewards is selected
- **THEN** every option SHALL render as an item icon with count and name

#### Scenario: No reward renders nothing
- **GIVEN** a quest with no reward definition in `quest_rewards.json`
- **THEN** no reward row SHALL be rendered
### Requirement: Quest lock reason display
The quest book SHALL explain why a LOCKED quest is locked, showing unmet prerequisites and/or era gating.

#### Scenario: Unmet prerequisites listed
- **GIVEN** a quest with status LOCKED and at least one prerequisite quest not COMPLETED
- **THEN** the detail view SHALL list each unmet prerequisite quest by title with its current status
- **AND** prerequisites that are COMPLETED SHALL NOT be listed as blockers

#### Scenario: Era gate shown
- **GIVEN** a quest with status LOCKED whose prerequisites are all COMPLETED but whose era is not yet unlocked
- **THEN** the detail view SHALL state that the quest's era is locked and which earlier era must be completed

#### Scenario: Locked with mixed blockers
- **GIVEN** a quest with status LOCKED having both unmet prerequisites and a locked era
- **THEN** both the unmet prerequisites and the era gate SHALL be shown
