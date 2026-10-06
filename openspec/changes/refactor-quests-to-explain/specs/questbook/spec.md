## MODIFIED Requirements

### Requirement: Quest Data Model
The system SHALL support a quest progression system with eras, sections, and individual quests.

#### Scenario: Quests are organized in eras
- **GIVEN** the quest data
- **THEN** quests are grouped into eras: Vagrant, Apprentice, Expert, Administrator
- **AND** eras contain sections (Foundation, Electric Tools, Machine Config, Transport...)
- **AND** sections contain individual quests

#### Scenario: Quest data loaded from CSV + JSON
- **GIVEN** the system starts
- **WHEN** the quest library loads data
- **THEN** it SHALL parse `data/quests/quests.csv` for quest definitions (id, title, description, era, section, prereqs, detect_type, detect_target, reward_item, reward_count)
- **AND** it SHALL parse the trailing `target_count` column (INVENTORY objective quantity; 0 → treated as ≥1)
- **AND** it SHALL parse the EXCHANGE columns (`cost_item`, `cost_count`, `cooldown`)
- **AND** it SHALL parse the optional `hint` and `lore` columns, treating an absent or empty value as empty prose
- **AND** it SHALL parse `data/quests/quest_graph.json` for DAG edges (prerequisites, position hints)
- **AND** it SHALL fail gracefully with a log warning if either file is missing or malformed

#### Scenario: Quest definition struct
- **GIVEN** a quest is defined
- **THEN** its definition SHALL include: unique id (uint32), title, description, era (enum: VAGRANT/APPRENTICE/EXPERT/ADMINISTRATOR), section name, prerequisites list, detection type (enum: CRAFT/BLOCK_PLACED/TOOL_CHARGED/SIDE_CONFIGURED/INVENTORY), detection target string, reward item id (uint16), reward count (uint8)
- **AND** its definition SHALL include an optional hint string and an optional lore string, both empty when the column is absent
- **AND** INVENTORY quests SHALL carry a `targetCount` (default 0 = ≥1)

#### Scenario: BuildEraStructure produces UI-ready hierarchy
- **GIVEN** quest data is loaded
- **WHEN** `QuestData::BuildEraStructure()` is called
- **THEN** it SHALL return a vector of `EraInfo`, each containing sections with their quest IDs
- **AND** the ordering SHALL be: Vagrant → Apprentice → Expert → Administrator

#### Scenario: Section carries an unlocks hint
- **GIVEN** a section declares an `unlocks_hint`
- **WHEN** the quest book renders the section header
- **THEN** the hint SHALL be visible to the player before they enter the section
- **AND** a section with no `unlocks_hint` SHALL render its header as before

#### Scenario: Explain quests carry prose instead of a target
- **GIVEN** a quest whose requirement kind is `explain`
- **THEN** its detection target SHALL be empty
- **AND** it SHALL NOT be evaluated by any of CRAFT, BLOCK_PLACED,
      TOOL_CHARGED, SIDE_CONFIGURED or INVENTORY
- **AND** its description SHALL be the authoritative payload

#### Scenario: Prose fields survive the wire
- **GIVEN** a quest with a hint, lore, or sub-steps
- **WHEN** the server sends it to the client over `QuestProgressUpdate`
- **THEN** the hint, lore and sub-step state SHALL be present in the FlatBuffer
- **AND** a payload produced by a client built against the schema without these
      fields SHALL still parse, yielding empty strings and an empty sub-step
      list rather than a parse failure