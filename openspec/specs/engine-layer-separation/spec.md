# engine-layer-separation Specification

## Purpose
TBD - created by archiving change refactor-engine-layer-separation. Update Purpose after archive.
## Requirements
### Requirement: Layered Source Layout

The codebase SHALL be organized into four source layers: `src/engine/` (engine-core), `src/game/` (game rules), `src/content/` (GTNH content pack), and `src/apps/` (assembly points). Engine-core code SHALL NOT reference game-rules or content symbols; game code SHALL NOT be included by engine targets.

#### Scenario: Engine builds without game or content
- **WHEN** engine targets (`gtnh_engine_registry`, `gtnh_engine_sim`, `gtnh-net`, storage interfaces) are built and their tests run
- **THEN** the build succeeds without linking any `src/game/` or `src/content/` target
- **AND** no engine header includes a game or content header

#### Scenario: Assembly point links all layers
- **GIVEN** the simulation application target
- **WHEN** it is linked
- **THEN** it links engine, game, and content targets
- **AND** the application main is the only place that wires concrete content registration

### Requirement: Static Library Modules

The engine SHALL be decomposed into multiple static libraries, each owning one mechanism: registry loading and item-id packing; simulation infrastructure (tick, pattern-matching machinery, condition mechanics); networking (io_uring, frames); storage interfaces. The final application SHALL link the modules it needs; no monolithic engine archive SHALL be created.

#### Scenario: Per-module targets exist
- **WHEN** the CMake target graph is inspected
- **THEN** each mechanism is a separate static library target
- **AND** services depend on explicit targets, not include-path-only usage

#### Scenario: One dependency direction
- **GIVEN** any game target
- **WHEN** its CMake dependencies are inspected
- **THEN** it links only engine targets and external packages
- **AND** no engine target links a game target

### Requirement: Content Pack Registration Boundary

GTNH-specific identifiers and balance values (fuel tables, machine-id constants, fluid definitions, ore ids) SHALL live in `src/content/` — as data files or in a single thin registration unit — and SHALL be passed into game systems at registration time. Game systems SHALL receive registered identifiers instead of hardcoding them.

#### Scenario: Boiler machine id resolves via registration
- **GIVEN** the boiler system checks machine identity
- **WHEN** it needs the machine id
- **THEN** it reads an id supplied by content registration
- **AND** no item-id literal is hardcoded in the system source

#### Scenario: Content is data plus thin registration
- **GIVEN** the content layer
- **THEN** bulk content (items, recipes, quests, patterns, fluids) is data files
- **AND** C++ in content is limited to a registration unit naming concrete ids and balance

### Requirement: Registry Loader Roles Preserved

Both existing item registry loaders SHALL be preserved in this change: the strict shared registry remains the canonical validated loader; the recipe library retains its consumer-side item registry. Their responsibilities SHALL be documented at the new paths.

#### Scenario: Strict loader stays canonical
- **WHEN** a service validates registry data at startup
- **THEN** it uses the strict shared registry from the engine registry module
- **AND** the recipe library continues to resolve item references through its own consumer registry without redefining the canonical catalog

### Requirement: Data Directory Relocation

World content data (`data/` trees) SHALL move under `src/content/data/` with loader paths and compile definitions updated. Item ids, wire format, and save formats SHALL NOT change in this change.

#### Scenario: Loaders read relocated data
- **WHEN** services start after the move
- **THEN** registry, recipe, and quest loaders read from `src/content/data/` paths
- **AND** all previously passing tests still pass
- **AND** packed item ids and protocol messages are unchanged

### Requirement: Mechanical Move Discipline

The first restructuring pass SHALL be performed by a script emitting `git mv` operations, with every affected file classified as MOVE (whole file relocates), EXTRACT (machinery stays, content specifics move to a new file), or KEEP (no move). EXTRACT files SHALL keep their mechanism code in the game layer and relocate content tables to the content layer.

#### Scenario: Rename history preserved
- **WHEN** the first pass completes
- **THEN** `git status` shows renames rather than delete/add pairs
- **AND** each file carries exactly one classification

### Requirement: Modding Scope Boundaries

Lua/Python mod runtimes and sidecar mod binaries SHALL remain permanently excluded. The dynamic-vs-VM runtime question (Q14) SHALL stay deferred; this change delivers only the source-level seam. Item-id namespace refactoring SHALL NOT be performed here.

#### Scenario: No runtime commitment
- **WHEN** this change lands
- **THEN** no `.so`/VM loading code is added
- **AND** registration entry points are the only future mod hook documented

