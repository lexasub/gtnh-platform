# Change: Unify Recipe Item ID Parsing

## Why
Recipe YAML `item:` fields may use any of three formats — hierarchical (`0:0:13`), flat numeric (`13`), or string name (`iron_ore`) — but the parser only handles flat and string reliably. The current `stoi → catch → resolveItemName` chain silently mis-parses hierarchical ids: `std::stoi("0:0:13")` returns `0` (stoi stops at the first colon), so a hierarchical id loads as item 0 with no error. Pattern-based format detection makes all three formats first-class and removes the ambiguity.

## What Changes
- **Parser**: RecipeManager YAML parser — replace the `stoi → catch → resolveItemName` fallback chain with a `resolveItemId()` format-detect helper: hierarchical (contains `:`), flat numeric (digits only), string name (else). Hierarchical and flat both call `ItemId::pack()`; names call `resolveItemName()`.
- **No fallback ambiguity**: each input format is detected by pattern, not by try/catch.
- **Fixes a latent bug**: hierarchical ids no longer silently parse as `0`.

## Impact
- Affected specs: recipe-id-format (new)
- Affected code:
  - `src/libs/recipe_manager_lib/RecipeManager.h` — `resolveItemId()` declaration
  - `src/libs/recipe_manager_lib/RecipeManager.cpp` — `parseYamlInputItem()`, `parseYamlOutputItem()` route through `resolveItemId()`
- Non-goals: no recipe data changes in this change. The legacy flat ids in `data/recipes/*.yaml` are block ids, not packed item ids, so a mechanical `ItemId::unpack()` migration would corrupt every recipe; recipe data migration to string names is owned by `fix-ore-processing-chain`.
