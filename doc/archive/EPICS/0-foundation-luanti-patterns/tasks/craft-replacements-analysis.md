# Craft Replacements — Analysis Task

## Objective
Implement `replacement` field in recipe input items, generalizing current `consume: false` behavior.

## Current State
`consume: false` skips consumption but doesn't add replacement items (lava bucket → empty bucket is broken).

## Deliverables
1. Updated JSON recipe format with `replacement: [item_id, count]`
2. Updated `Recipe::craft()` logic in RecipeManager
3. Updated parser in `parseCompactInputList()`
4. Updated furnace.json to use replacement for buckets

## Constraints
- Backward compatible: old recipes without `replacement` work as before
- Replacements are added to the container after consumption, not instead of it
- Multiple replacements per input allowed
