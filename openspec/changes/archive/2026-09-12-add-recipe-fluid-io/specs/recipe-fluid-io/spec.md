## ADDED Requirements

### Requirement: Fluid Inputs in Recipes
Recipes SHALL be able to declare `fluid_inputs` — fluids consumed per operation, expressed as `{ fluid: <name>, amount: <mB> }` pairs. Fluid names SHALL resolve through the canonical item registry (same rules as item names in recipe-id-format) and SHALL additionally resolve to a row in `fluids.csv`; otherwise recipe validation SHALL fail.

#### Scenario: Fluid input declared by name
- **GIVEN** a recipe YAML with `fluid_inputs: [{ fluid: water, amount: 2000 }]`
- **WHEN** the recipe is loaded
- **THEN** the parser resolves `water` to its packed items.csv id
- **AND** validates that the id has a fluids.csv row
- **AND** stores the pair as (fluid_id, 2000) on the Recipe

#### Scenario: Unknown fluid name fails validation
- **GIVEN** a recipe YAML with `fluid_inputs: [{ fluid: not_a_fluid, amount: 100 }]`
- **WHEN** the recipe is loaded
- **THEN** loading fails and an error names the offending fluid
- **AND** no partially-parsed recipe is registered

#### Scenario: Zero amount rejected
- **GIVEN** a recipe YAML with `fluid_inputs: [{ fluid: water, amount: 0 }]`
- **WHEN** the recipe is loaded
- **THEN** validation rejects the requirement as invalid

### Requirement: Fluid Outputs in Recipes
Recipes SHALL be able to declare `fluid_outputs` — fluids produced per operation, expressed as `{ fluid: <name>, amount: <mB> }` pairs with the same resolution and validation rules as fluid inputs.

#### Scenario: Fluid output declared by name
- **GIVEN** a recipe YAML with `fluid_outputs: [{ fluid: hydrogen, amount: 2000 }, { fluid: oxygen, amount: 1000 }]`
- **WHEN** the recipe is loaded
- **THEN** both pairs resolve to packed fluids.csv-backed item ids
- **AND** are stored on the Recipe as fluid outputs

### Requirement: Machine Reservations for Fluid Inputs
Before a recipe with `fluid_inputs` starts, the machine SHALL reserve the full per-operation amount of each input fluid through the existing resource-reservation contract (4.1.4); item inputs SHALL NOT be consumed until all fluid reservations are fully accepted.

#### Scenario: Craft waits for fluid reservation
- **GIVEN** a machine with an idle recipe that declares `fluid_inputs: water 2000`
- **WHEN** the machine has only 500 mB water in its FluidStorage
- **THEN** the craft stays pending
- **AND** no item inputs are consumed

#### Scenario: Craft starts after full reservation
- **GIVEN** the same machine, now with >= 2000 mB water available
- **WHEN** the reservoir accepts the full 2000 mB
- **THEN** the craft begins
- **AND** item inputs are consumed exactly once

### Requirement: Fluid Outputs Credited on Completion
When a recipe with `fluid_outputs` completes, the produced fluids SHALL be credited into the machine's fluid output storage; the machine SHALL then expose them for drain through its FLUID source port.

#### Scenario: Output fluids enter machine tank
- **GIVEN** a recipe with `fluid_outputs: hydrogen 2000, oxygen 1000` completes
- **THEN** the machine's FluidStorage gains 2000 mB hydrogen and 1000 mB oxygen
- **AND** the amounts are observable via the ResourceBufferState publisher
