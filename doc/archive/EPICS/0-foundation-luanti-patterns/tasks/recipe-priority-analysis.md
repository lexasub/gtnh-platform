# Craft Priority System — Analysis Task

## Objective
Add priority-based recipe matching to replace current first-match-wins behavior.

## Current State
`recipesByMachineType_` = `unordered_map<MachineType, vector<string>>`. Linear scan, first match wins.

## Deliverables
1. `priority` field in Recipe struct (uint8, default=0)
2. Priority levels: `OVERRIDE=100 > SHAPED=50 > SHAPELESS=30 > FALLBACK=10 > DEFAULT=0`
3. Hash index for fast item_name lookup (optional, performance)
4. Update `findRecipeByInputs()` to sort by priority descending
5. Update JSON parser to read optional `pri` field

## Constraints
- `priority` is optional — existing recipes default to 0 (DEFAULT)
- Higher priority always wins over lower, regardless of registration order
- Same priority → first registered wins (current behavior)
- Shaped/shaleless distinction deferred (only priority for now)
