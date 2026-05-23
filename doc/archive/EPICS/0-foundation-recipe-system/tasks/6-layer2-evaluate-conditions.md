# TASK: Layer 2 — EvaluateConditions & Categories
**Layer**: 2
**Status**: Draft
**Epic**: 0-foundation-recipe-system

## Affected Services

| Service | Role | Direction |
|---------|------|-----------|
| **RecipeManager** ⬅️ NEW | Server — EvaluateConditions RPC, category registry | Responds |
| **SimulationCore** | Client — sends MachineState for evaluation | Calls |
| **MessageRouter** | Transport | — |



## Overview

This document specifies the Layer 2 extension to the Recipe Manager system, defining the `EvaluateConditions` RPC and the extensible category system for machine recipes. Layer 2 builds upon the core recipe execution model from Layer 1 by adding conditional logic and domain-specific categorization.

### Core Concept

Recipes are no longer simple input/output transforms. They may require specific environmental conditions to execute successfully. The `EvaluateConditions` RPC provides a uniform interface for evaluating these conditions against the current machine state.

## EvaluateConditions RPC

The `EvaluateConditions` RPC determines whether a recipe may proceed based on dynamic conditions. This separates recipe validation from recipe execution, allowing the same recipe to behave differently depending on runtime state.

### Signature

| EvaluateConditions | recipe_id: string, machine_state: MachineState | bool |

**Parameters:**
- `recipe_id`: The unique identifier of the recipe to evaluate
- `machine_state`: The current state of the machine, including temperature, liquids, energy, purity, biome, coordinates, and tick progress

**Returns:**
- `true` if all conditions pass and the recipe may proceed
- `false` if any condition fails, blocking recipe execution

### Evaluation Flow

1. RecipeManager calls `EvaluateConditions(recipe_id, machine_state)`
2. Conditions are evaluated in order until one fails
3. On failure:
   - Recipe execution is aborted immediately
   - A condition failure message is logged with the failing condition name
   - The failed recipe is not added to the active recipe list
4. On success:
   - Recipe execution proceeds normally
   - A condition pass event is logged for debugging

### Condition Evaluation Order

Conditions must be evaluated in a consistent order to ensure deterministic behavior. The default evaluation order is:

1. Category match (recipe must belong to a supported category)
2. Temperature condition
3. Energy condition
4. Liquid condition
5. Purity condition
6. Biome condition
7. Location condition
8. Progress condition

## MachineState Structure

`MachineState` contains the current runtime data of a machine. This structure is populated by the machine's `getMachineState()` method and passed to RecipeManager for condition evaluation.

### Fields

| Field | Type | Description |
|-------|------|-------------|
| temperature | float | Current machine temperature in Kelvin |
| energy | float | Current energy stored (in Joules or machine-specific units) |
| liquid_levels | map<string, float> | Current liquid levels by type (e.g., `{"water": 0.5, "molten_iron": 0.8}`) |
| purity | float | Current purity level (0.0 to 1.0) |
| biome | string | Current biome name (e.g., "deepslate", "nether", "overworld") |
| coordinates | struct { x: int32, y: int32, z: int32, dim: string } | Current world coordinates and dimension |
| progress | float | Current duration progress as a fraction (0.0 to 1.0) |

### Example

```
{
  temperature: 2500.0,
  energy: 100000.0,
  liquid_levels: {
    "molten_iron": 0.8,
    "water": 0.3
  },
  purity: 0.95,
  biome: "deepslate",
  coordinates: {
    x: -1234,
    y: 64,
    z: 5678,
    dim: "overworld"
  },
  progress: 0.75
}
```

## Category Extension System

Categories organize recipes by domain and determine which conditions are relevant to each machine type. The system is designed to be extensible: new categories can be added without modifying existing category implementations.

### Built-in Categories

| Category | Machines | Conditions |
|----------|----------|------------|
| smelting | Furnace, BlastFurnace, Smoker | Temperature, Energy, Liquid, Purity, Location |
| chemical_reactor | ChemicalReactor, AlchemicalTablet | Temperature, Energy, Liquid, Purity, Biome, Progress |
| machining | Macerator, CrushingOre, Pulverizer | Energy, Temperature, Purity |
| assembly_line | AssemblyTable, AssemblyMachine | Energy, Liquid, Purity |
| distillation | Distiller, AlcoholStill | Temperature, Energy, Liquid, Purity, Progress |
| brewing | BrewingStand, Brewery | Temperature, Liquid, Progress |
| electrolysis | Electrolyser, ElectrolysisMachine | Energy, Liquid, Purity, Temperature |

### Category Structure

Each category is a first-class object containing:
- `id`: The category identifier
- `machines`: List of machine IDs that belong to this category
- `conditions`: List of condition types relevant to this category
- `evaluate`: Function that evaluates category-specific conditions

### Extension Protocol

To add a new category:
1. Define the category ID and list of machines
2. Specify which conditions are relevant
3. Implement the evaluation logic for those conditions
4. Register the category with the RecipeManager

The RecipeManager automatically discovers and loads categories at startup. Categories are loaded in alphabetical order by ID to ensure deterministic evaluation order.

## Condition Evaluation Pipeline

The pipeline orchestrates condition evaluation from RPC invocation to final decision.

### Pipeline Steps

```
┌─────────────┐
│  RPC Call   │
│EvaluateConditions│
└──────┬──────┘
       │
       ▼
┌─────────────────┐
│  Recipe Lookup  │
│  Find recipe by │
│  recipe_id      │
└──────┬──────────┘
       │
       ▼
┌─────────────────┐
│  Category Lookup│
│  Get category   │
│  for recipe     │
└──────┬──────────┘
       │
       ▼
┌─────────────────┐
│  Condition      │
│  Evaluation     │
│  (in order)     │
└──────┬──────────┘
       │
       ▼
┌─────────────────┐
│  Result         │
│  true / false   │
└─────────────────┘
```

### Pipeline Implementation

```cpp
bool EvaluateConditions(const std::string& recipe_id, const MachineState& state) {
    // Step 1: Find recipe
    auto recipe = RecipeManager::Get(recipe_id);
    if (!recipe) {
        Log("Recipe not found: {0}", recipe_id);
        return false;
    }

    // Step 2: Get category
    auto category = RecipeManager::GetCategory(recipe->category_id);
    if (!category) {
        Log("Unknown category: {0}", recipe->category_id);
        return false;
    }

    // Step 3: Evaluate conditions
    for (const auto& condition : recipe->conditions) {
        if (!EvaluateSingleCondition(state, condition)) {
            Log("Condition failed: {0}", condition.name);
            return false;
        }
    }

    return true;
}
```

### Single Condition Evaluation

Each condition type has its own evaluator:

```cpp
bool EvaluateSingleCondition(const MachineState& state, const Condition& condition) {
    switch (condition.type) {
        case ConditionType::Temperature:
            return EvaluateTemperature(state, condition);

        case ConditionType::Energy:
            return EvaluateEnergy(state, condition);

        case ConditionType::Liquid:
            return EvaluateLiquid(state, condition);

        case ConditionType::Purity:
            return EvaluatePurity(state, condition);

        case ConditionType::Biome:
            return EvaluateBiome(state, condition);

        case ConditionType::Location:
            return EvaluateLocation(state, condition);

        case ConditionType::Progress:
            return EvaluateProgress(state, condition);

        default:
            Log("Unknown condition type: {0}", static_cast<int>(condition.type));
            return false;
    }
}
```

### Condition Specifications

Each condition is defined by a spec containing the type, operator, threshold, and optional target.

| Type | Operator | Threshold | Target |
|------|----------|-----------|--------|
| Temperature | >= | float | float |
| Temperature | <= | float | float |
| Temperature | == | float | float |
| Energy | >= | float | float |
| Energy | <= | float | float |
| Energy | == | float | float |
| Liquid | >= | float | string |
| Liquid | <= | float | string |
| Purity | >= | float | float |
| Purity | <= | float | float |
| Purity | == | float | float |
| Biome | == | string | string |
| Biome | contains | string | string |
| Location | == | struct { x, y, z, dim } | struct { x, y, z, dim } |
| Location | dim == | string | string |
| Progress | >= | float | float |
| Progress | <= | float | float |
| Progress | == | float | float |

## File Locations

| File | Path |
|------|------|
| EvaluateConditions RPC | `src/services/simulation_core/recipe/evaluate_conditions.hpp` |
| MachineState Structure | `src/services/simulation_core/recipe/machine_state.hpp` |
| Category System | `src/services/simulation_core/recipe/category.hpp` |
| Condition Pipeline | `src/services/simulation_core/recipe/condition_pipeline.hpp` |
| Condition Evaluator | `src/services/simulation_core/recipe/condition_evaluator.hpp` |

## Acceptance Criteria

#### Scenario: Temperature Condition Passes

**Given** a smelting recipe requiring minimum 1500 K
**When** the furnace reaches 1800 K
**Then** EvaluateConditions returns true
**And** the recipe is added to the active recipe list

#### Scenario: Category-Based Lookup

**Given** a chemical reactor recipe in the chemical_reactor category
**When** a recipe is executed and conditions are evaluated
**Then** only chemical_reactor-relevant conditions are checked
**And** smelting-specific conditions (like furnace location) are skipped

#### Scenario: Condition Failure Blocks Execution

**Given** a recipe requiring 90% purity
**When** the purity is only 85%
**Then** EvaluateConditions returns false
**And** the condition failure is logged
**And** the recipe is not added to the active recipe list
**And** the output is not consumed
**And** the inputs are returned to the inventory

## Open Questions

### How does RecipeManager know temperature, liquids, purity, biome?

The RecipeManager does not measure these values itself. Instead, it relies on the machine to provide accurate data through the `getMachineState()` method. The machine's internal sensors measure the values, and the machine constructs the `MachineState` struct before passing it to RecipeManager.

**Proposed solution:** The machine's `getMachineState()` method returns a snapshot of its current state. This snapshot is constructed from the machine's internal data:
- Temperature is read from the thermal sensor
- Liquid levels are queried from the fluid storage
- Purity is measured by the analyzer
- Biome and coordinates are obtained from the world
- Progress is tracked by the recipe engine

The RecipeManager trusts the machine to provide accurate data. If the machine lies, the conditions will not match, and the recipe will fail naturally.

### How are new categories discovered?

Categories are discovered at startup through a plugin system. Each category is defined in a header file in `src/services/simulation_core/recipe/categories/`. The RecipeManager uses reflection to find and load all categories.

**Proposed solution:** Categories are compiled into the binary. Each category declares itself with a macro, and the RecipeManager scans for these declarations at startup. This avoids the overhead of dynamic loading while keeping the system extensible.

## References

- Epic spec: `doc/EPICS/0-foundation-recipe-system/0-foundation-recipe-system.md`
- Layer 1 spec: `doc/EPICS/0-foundation-recipe-system/tasks/1-core-recipe-engine.md`
- MachineState definition: `doc/EPICS/0-foundation-recipe-system/tasks/2-machine-state.md`
