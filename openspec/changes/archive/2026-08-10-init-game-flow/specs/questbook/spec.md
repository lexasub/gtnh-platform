# questbook Specification

## Purpose

Define the quest book UI, quest data model, quest protocol, storage, completion detection, DAG unlock logic, reward distribution, and scenario-driven opening.

## ADDED Requirements

### Requirement: Quest Book Opens on Scenario Start

The client SHALL open the Quest Book programmatically when a game scenario starts successfully.
On a successful `StartScenarioResp`, the client SHALL open `QuestBookWindow` and select the quest
era carried in the response. The default era selection (VAGRANT = 0) SHALL remain the no-op default
for the initial scenario.

#### Scenario: Scenario start opens the book on Vagrant

- **GIVEN** the client receives `StartScenarioResp(success = true, quest_book_era = VAGRANT)`
- **WHEN** it processes the response
- **THEN** `QuestBookWindow` SHALL be opened if not already open
- **AND** the selected era SHALL be VAGRANT, showing the vagrant section tabs

#### Scenario: Failed scenario does not open the book

- **GIVEN** the client receives `StartScenarioResp(success = false, error = ...)`
- **WHEN** it processes the response
- **THEN** `QuestBookWindow` SHALL NOT be opened by the scenario
- **AND** the error SHALL be printed to the console
