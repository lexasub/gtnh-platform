# TASK: Layer 2 — Conditional Recipes
**Layer**: 2
**Status**: Draft
**Epic**: 0-foundation-recipe-system

## Affected Services

| Service | Role |
|---------|------|
| **RecipeManager** ⬅️ NEW | Primary — extended JSON schema with temperature/liquid/purity/biome conditions |
| **SimulationCore** | Consumer — supplies MachineState for EvaluateConditions |



## Overview

Layer 2 extends the base recipe system with GregTech-style conditional execution. Recipes now support machine-specific constraints: temperature ranges, energy requirements, liquid component presence, purity thresholds, biome restrictions, and arbitrary special conditions.

Recipes are categorized by machine type to apply category-appropriate validation rules.

## Expanded JSON Format

```json
{
  "id": "gt:smelting:electrum_ingot",
  "category": "smelting",
  "inputs": [],
  "outputs": [{"item": "electrum_ingot", "count": 1}],
  "machine": "furnace",
  "temperature": {"min": 1300, "max": 1500},
  "energy_cost": 0.5,
  "duration": 200,
  "liquid_components": [{"liquid": "flux", "amount": 100}]
}
```

### Field Definitions

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | yes | Unique recipe identifier |
| `category` | string | yes | Machine category |
| `inputs` | ItemStack[] | yes | Input items (may be empty) |
| `outputs` | ItemStack[] | yes | Output items |
| `machine` | string | yes | Machine type identifier |
| `temperature` | TemperatureRange | no | Min/max temperature in °C |
| `energy_cost` | number | no | Energy per tick |
| `duration` | number | no | Duration in ticks |
| `liquid_components` | Liquid[] | no | Required liquids |
| `purity` | number | no | Purity threshold 0-100 |
| `biome` | string[] | no | Required biomes |
| `special_condition` | string | no | Arbitrary condition |

## Condition Types

### Temperature
Applies to heat-based machines (smelting, chemical reactor). Min and/or max values in °C. Both inclusive.

### Energy Cost
Applies to power-consuming machines. 0 means energy is not a constraint.

### Duration
Number of ticks until completion. Applies to all recipe types.

### Liquid Components
Required liquids with minimum volume. All must be present simultaneously.

### Purity
Value 0-100. Recipe requires at least this purity level.

### Biome
Recipe only executes when machine is in a listed biome. Multiple biomes are OR conditions.

### Special Condition
Catch-all for platform-specific conditions: `player_nearby`, `time_of_day:night`, `dimension:overworld`.

## Category System

| Category | Valid Conditions | Examples |
|----------|-----------------|----------|
| smelting | temperature, energy_cost, duration, liquid_components | Furnace, blast furnace |
| chemical_reactor | temperature, energy_cost, duration, liquid_components, purity, special_condition | Chemical reactor, distillery |
| machining | energy_cost, duration, special_condition | Macerator, compressor |
| assembly_line | energy_cost, duration, liquid_components, special_condition | Assembly line |

## Acceptance Criteria

#### Scenario: Smelting electrum ingot
A furnace operates at 1400°C with 50 flux in its tank. Temperature (1400°C) falls within [1300, 1500], flux is present at 100 mL (≥ 100 required). Condition evaluates to true.

#### Scenario: Chemical reactor with purity requirement
A chemical reactor holds sulfuric acid at 95% purity. Recipe requires purity: 90. Comparison 95 ≥ 90 returns true. If purity drops to 89, next evaluation returns false.

#### Scenario: Biome restriction
A grinder sits in a jungle biome. Recipe has biome: ["jungle", "swamp"]. Check passes. If moved to desert, next evaluation fails.

#### Scenario: Temperature outside range
A furnace at 1600°C. Recipe requires temperature: {min: 1300, max: 1500}. Since 1600 > 1500, condition fails.
