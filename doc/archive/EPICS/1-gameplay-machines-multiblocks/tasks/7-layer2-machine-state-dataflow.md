# Layer 2 Conditions: MachineState → real ECS data

## Objective
Wire real ECS data into ConditionEvaluator. Currently `evaluateConditions()` is called with empty `MachineState{}` — code exists, data doesn't.

## Current State
- `ConditionEvaluator` (152 строк) — fully implemented, checks environment (temperature, purity, biome), machine (energy, network, facing), special tags
- `RecipeManager::evaluateConditions(recipeId)` — bare overload, passes empty `MachineState{}`
- `RecipeManager::evaluateConditions(recipeId, reg, x, y, z)` — queries ECS for `MachineComponent`, `EnergyStorage`, `Block` — partially populates state
- Missing: temperature, purity, biomes, fluid_slots, network_ids

## Required Changes

### 1. SimulationCore ECS Components
Ensure machines have the necessary ECS components:
- `MachineComponent` — position, type ✓ (exists)
- `EnergyStorage` — current energy, capacity ✓ (exists)
- `Block` — block_id, meta ✓ (exists)
- `MachineEnvironment` (NEW) — temperature, purity, biome_id
- `FluidTank` (NEW) — fluid_id, amount, capacity

### 2. MachineState Population
`RecipeManager::evaluateConditions(recipeId, reg, x, y, z)` must populate:
- `state.temperature` → from `MachineEnvironment.temperature` (or world biome default)
- `state.purity` → from `MachineEnvironment.purity`
- `state.biome_id` → from `MachineEnvironment.biome_id`
- `state.energy` → from `EnergyStorage.current` ✓ (already done)
- `state.fluid_slots` → uncomment vector, populate from `FluidTank` components
- `state.facing` → from `Block.meta` ✓ (already done)

### 3. Data Sources (resolve Open Questions Q3-Q6)
| Field | Source | Resolution |
|-------|--------|------------|
| Temperature | MachineEnvironment.temperature | NEW component, default = biome ambient temp |
| Purity | MachineEnvironment.purity | NEW component, default = 1.0 |
| Biome | SpatialIndex or MachineEnvironment | SpatialIndex is stub — set from MachineEnvironment |
| Fluids | FluidTank components | NEW component on machine entity |
| Energy | EnergyStorage | Already exists |

### 4. EvaluateConditions RPC
Currently `recipe.fbs` defines `EvaluateConditionsReq { recipe_id, machine_state }` but:
- The no-arg REST overload passes empty MachineState
- The ECS overload bypasses RPC entirely (direct call)
- Decision: keep direct ECS call for hot-path, RPC for remote clients

## Constraints
- All new ECS components optional — machines without them use defaults (temperature=20°C, purity=1.0, biome=plains)
- MachineEnvironment default values must not break existing recipes
- FluidTank is separate from MachineEnvironment (fluids change dynamically)
- EvaluateConditions RPC kept for future standalone use, but hot-path always uses ECS overload

## Test Requirements
- Machine with EnergyStorage + MachineEnvironment → ConditionEvaluator sees correct values
- Machine without MachineEnvironment → defaults used, recipe with no conditions still passes
- Machine with temperature out of recipe range → evaluate returns false
- Craft still works when conditions pass
