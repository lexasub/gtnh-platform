## ADDED Requirements

### Requirement: Quest reward data from JSON
The system SHALL load quest rewards from a dedicated `data/quests/quest_rewards.json` file rather than the flat `reward_item`/`reward_count` columns in `quests.csv`.

#### Scenario: Rewards loaded from JSON
- **GIVEN** `data/quests/quest_rewards.json` exists and is well-formed
- **WHEN** the client quest book loads quest data
- **THEN** each quest's rewards SHALL be read from the JSON file, keyed by quest id
- **AND** item rewards SHALL pack their `item` spec string via `ItemId::pack` for icon lookup

#### Scenario: Multiple rewards per quest
- **GIVEN** a quest with a `rewards` array containing more than one entry
- **THEN** all entries SHALL be available for rendering and reward resolution

#### Scenario: Choice rewards
- **GIVEN** a quest with a `choice_of` array
- **THEN** all choice options SHALL be available for rendering
- **AND** a quest SHALL define either `rewards` or `choice_of`, not both (validated)

#### Scenario: Missing or malformed JSON
- **GIVEN** `quest_rewards.json` is missing or malformed
- **THEN** the system SHALL log a warning
- **AND** quests SHALL render with no reward rows rather than fail

### Requirement: Quest requirement data from JSON
The system SHALL load quest requirements from a dedicated `data/quests/quest_requirements.json` file rather than the flat `detect_type`/`detect_target`/`target_count` columns in `quests.csv`.

#### Scenario: Requirements loaded from JSON
- **GIVEN** `data/quests/quest_requirements.json` exists and is well-formed
- **WHEN** the client quest book loads quest data
- **THEN** each quest's requirements SHALL be read from the JSON file, keyed by quest id
- **AND** each requirement's `item` spec string SHALL pack via `ItemId::pack` for icon lookup

#### Scenario: Requirement kinds
- **GIVEN** a quest with requirements of kinds `craft`, `obtain`, `place`, or `machine`
- **THEN** each requirement SHALL carry its kind
- **AND** a `machine` requirement SHALL carry a `machine` item spec

#### Scenario: Consume flag
- **GIVEN** a requirement with `consume: true`
- **THEN** the requirement SHALL be marked as taken on completion
- **AND** a requirement with `consume: false` SHALL be marked as kept

#### Scenario: Missing or malformed JSON
- **GIVEN** `quest_requirements.json` is missing or malformed
- **THEN** the system SHALL log a warning
- **AND** quests SHALL render with no requirement rows rather than fail

### Requirement: Quest auto-completion control
The system SHALL let each quest opt in to instant completion when its requirement is met, or require manual completion via the Complete button.

#### Scenario: Auto-complete quest
- **GIVEN** a quest with `auto_complete: true` whose requirement is met and prerequisites are satisfied
- **THEN** the quest SHALL transition to COMPLETED immediately
- **AND** rewards SHALL be granted through the existing completion flow

#### Scenario: Manual-complete quest
- **GIVEN** a quest with `auto_complete: false` whose requirement is met
- **THEN** the quest SHALL transition to AVAILABLE (not COMPLETED)
- **AND** the quest SHALL complete only when the player presses the Complete button

#### Scenario: Auto-complete default
- **GIVEN** a quest with no `auto_complete` field in `quest_requirements.json`
- **THEN** the quest SHALL behave as `auto_complete: true`

### Requirement: Quest machine-output detection
The system SHALL detect quests of kind `machine` — obtaining an item from a machine of a specified type.

#### Scenario: Item obtained from machine
- **GIVEN** a quest with a requirement of kind `machine` for item X in machine Y
- **WHEN** a player takes item X from the output of a machine of type Y
- **THEN** the quest SHALL be evaluated for completion through the machine-output detection path

#### Scenario: Machine attribution fallback
- **GIVEN** a machine-output take event lacks the machine type
- **THEN** the quest SHALL fall back to INVENTORY-style detection (item held ≥ required count)

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
