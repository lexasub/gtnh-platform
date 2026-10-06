## 1. OpenSpec and contracts

- [x] 1.1 Update `texture-system` requirements for IDs 0–1023 and a 512×512, 32×32 atlas.
- [x] 1.2 Add scenarios for capacity bounds and self-merge slot allocation; validate the change strictly.

## 2. Runtime atlas

- [x] 2.1 Expand `TextureAtlas` grid, per-tile tables, slot counter, and checkerboard bookkeeping to 1,024 slots.
- [x] 2.2 Preserve source metadata but avoid standalone allocation for self-merge overlays; guard source, merge, face, item, and transport references and physical slot allocation.
- [x] 2.3 Verify self-merge and ordinary merge output, UVs, and safe capacity boundaries with existing runtime/test support.

## 3. Authoring and documentation

- [x] 3.1 Raise additive scanner tile limit and diagnostics to 1,023; add boundary/overflow tests.
- [x] 3.2 Update the texture-atlas format and capacity documentation.

## 4. Verification

- [x] 4.1 Run scanner tests and an idempotent dry run over the current texture data.
- [x] 4.2 Build the game client incrementally and run focused client tests plus configured CTest checks.
- [x] 4.3 Validate all source, merge, item, block-face, and transport references and check the final diff without disturbing unrelated changes.
