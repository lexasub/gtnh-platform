## 1. Parser

- [x] 1.1 Add a `resolveItemId(const std::string&)` helper to `RecipeManager` that format-detects the `item:` scalar: contains `:` → `ItemId::pack()` (hierarchical), all digits → `ItemId::pack()` (flat, backward compat), else → `resolveItemName()`. No try/catch for format detection.
- [x] 1.2 Replace the `stoi`→`catch`→`resolveItemName` chain in both `parseYamlInputItem()` and `parseYamlOutputItem()` with `resolveItemId()`.

## 2. Verification

- [x] 2.1 Build: `cd cmake-build-debug && ninja -j5` — no new compilation errors.
- [x] 2.2 Run `ctest --output-on-failure -j$(nproc)` — recipe tests pass.
- [x] 2.3 Confirm the three formats converge on identical `uint16_t`: `item: 0:0:4`, `item: 4`, and `item: glass` all resolve to the same value.
- [x] 2.4 Confirm hierarchical ids no longer parse as `0` (previous `std::stoi("0:0:4")` silent-zero bug).

## Out of scope (removed from original proposal)

- Flat→hierarchical **data** migration in `data/recipes/*.yaml` is removed. The flat recipe ids are legacy block ids, not packed item ids — a mechanical `ItemId::unpack()` rewrite would corrupt every recipe (e.g. `item: 3` "iron_ore" → `0:3` "sand"). Recipe data migration to string names is owned by `fix-ore-processing-chain`.
- The "Migration to Hierarchical Preferred" spec requirement is removed for the same reason.
