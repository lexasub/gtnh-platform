# Change: Refactor texture generation into an additive scanner

## Why

The original texture generator rebuilds packs and reconstructs CSV registries from hardcoded ID/name lists. That is unsafe now that texture mappings are curated manually: running it can overwrite manual face mappings, item overrides, merge recipes, pack pixels, and row ordering.

## What Changes

- Replace the destructive rebuild workflow with a dry-run-by-default additive scanner.
- Discover raw artwork dynamically and append only validated new pack cells and registry rows.
- Preserve existing CSV bytes/order and existing pack pixels.
- Use exact registry-name matches and optional explicit metadata; skip ambiguous artwork.
- Preserve existing merge recipes and only append declarative new recipes.

## Impact

- Affected capability: `texture-system`.
- Affected tooling: `tools/generate_texture_mappings.py`.
- Runtime code is unchanged; the scanner is an offline authoring tool.
