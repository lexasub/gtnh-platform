## ADDED Requirements

### Requirement: Separate logical groups from allocation prefixes

The item registry tooling SHALL represent a logical group path separately from an optional canonical packed-ID allocation prefix. Logical paths MAY have arbitrary depth; only an allocation prefix is limited to at most 15 binary bits. A manifest SHALL require an explicit `item_ids` list for every group; an empty list SHALL mean explicit empty membership, not inferred membership.

#### Scenario: Deep logical path shares an ancestor allocation pool
- **GIVEN** a group path `base/materials/metals/plates/copper`
- **AND** its nearest allocated ancestor has prefix `0:110:10`
- **AND** the group's explicit `item_ids` list contains its assigned items
- **WHEN** the manifest is validated
- **THEN** the logical path is accepted without allocating a new prefix
- **AND** capacity is reported against the ancestor allocation pool

#### Scenario: Parent and child use nested allocation ranges
- **GIVEN** parent allocation prefix `0:110`
- **AND** child allocation prefix `0:110:10`
- **WHEN** the allocation tree is validated
- **THEN** the child range is recognized as contained by the parent range
- **AND** the child range is reserved from the parent's direct free space

### Requirement: Canonicalize allocation prefixes with a dedicated prefix grammar

The tooling SHALL parse allocation prefixes separately from complete item IDs. An allocation prefix SHALL contain only binary prefix segments and no payload segment; the tooling SHALL concatenate those segments before comparing, allocating, or calculating capacity. To validate a complete item ID, a decimal payload SHALL be appended and then passed through the runtime packing rule. Separator placement SHALL NOT create a distinct allocation range.

#### Scenario: Equivalent textual prefixes share one range
- **GIVEN** allocation prefixes `0:1:0` and `0:10`
- **WHEN** they are parsed as allocation prefixes
- **THEN** both produce canonical bits `010`
- **AND** they SHALL be reported as aliases or a duplicate allocation declaration
- **AND** they SHALL NOT be treated as independent sibling ranges

#### Scenario: Invalid prefix is rejected
- **GIVEN** an allocation prefix containing a character other than `0` or `1`
- **WHEN** strict validation runs
- **THEN** validation SHALL fail with the source path and prefix
- **AND** no ID SHALL be allocated from that prefix

### Requirement: Calculate capacity from address ranges

For a canonical allocation prefix of length `L`, the tooling SHALL report capacity `2^(16-L)` and payload values from `0` through `2^(16-L)-1`. Capacity reporting SHALL distinguish direct item usage, child reservations, descendant usage, and free address ranges from logical item counts.

#### Scenario: Capacity boundary is enforced
- **GIVEN** an allocation prefix with 10 canonical bits
- **WHEN** an item requests payload `64`
- **THEN** validation SHALL reject the item because the valid payload range is `0..63`

#### Scenario: Parent has direct items and child reservations
- **GIVEN** a parent allocation range with direct items
- **AND** one child allocation range reserved inside the parent
- **WHEN** statistics are calculated
- **THEN** direct free space SHALL exclude the child range
- **AND** the parent SHALL not require an artificial `misc` group
- **AND** logical descendant item counts SHALL be reported separately from address capacity

### Requirement: Preserve existing IDs during allocation and merge

Allocation and merge tooling SHALL preserve all existing item IDs and SHALL fail rather than silently move an existing item when no compatible free range is available.

#### Scenario: New group fits in a free prefix range
- **GIVEN** existing items and child ranges leave an aligned free range inside a parent
- **WHEN** a dry-run allocation requests a child range with configured headroom
- **THEN** the tool SHALL propose a non-overlapping prefix block
- **AND** existing item IDs SHALL remain unchanged
- **AND** the dry run SHALL not modify source files

#### Scenario: Allocation would require relocation
- **GIVEN** all suitable ranges overlap existing items or child reservations
- **WHEN** allocation is requested
- **THEN** the tool SHALL return a non-zero failure
- **AND** it SHALL identify the conflicting ranges
- **AND** it SHALL not rewrite any item ID

### Requirement: Validate logical and allocation relationships

Strict validation SHALL reject overlapping sibling allocation ranges, allocation children outside their declared parent range, duplicate logical paths, duplicate packed item IDs, and payload values that overflow their canonical prefix. A logical subgroup without an allocation prefix SHALL be allowed to share its nearest allocated ancestor.

#### Scenario: Sibling ranges overlap
- **GIVEN** two children declared under one logical parent
- **AND** their canonical allocation ranges overlap
- **WHEN** validation runs
- **THEN** validation SHALL fail and identify both group paths

#### Scenario: Logical-only subgroup is valid
- **GIVEN** a unique logical path with no allocation prefix
- **AND** its parent has a valid allocation prefix
- **WHEN** validation runs
- **THEN** the subgroup SHALL share the nearest ancestor pool
- **AND** it SHALL not reserve a second copy of that range
