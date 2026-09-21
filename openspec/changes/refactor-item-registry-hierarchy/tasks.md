## 1. Specification and model

- [ ] 1.1 Review and approve the group manifest format and authoritative root namespaces.
- [x] 1.2 Add pure model helpers for canonical prefix parsing, packed range bounds, payload capacity, prefix containment, and interval subtraction.
- [x] 1.3 Add manifest parsing/serialization with normalized logical paths and explicit optional allocation prefixes.
- [ ] 1.4 Define deterministic sizing/headroom rules for new child allocation blocks.

## 2. Strict validation and allocation

- [x] 2.1 Validate current item IDs against runtime-compatible prefix and payload rules.
- [x] 2.2 Detect packed-ID collisions, duplicate allocation ranges, overlapping siblings, and invalid parent/child relations.
- [x] 2.3 Calculate direct usage, child reservations, descendant usage, free intervals, and growth headroom.
- [ ] 2.4 Implement dry-run allocation that preserves existing IDs and fails rather than relocating items.
- [ ] 2.5 Implement dry-run merge classification for aliases, packed-ID conflicts, name conflicts, and malformed draft rows.

## 3. CLI and GUI

- [ ] 3.1 Add CLI subcommands for `validate`, `stats`, `allocate`, and `merge --dry-run`.
- [x] 3.2 Make `tools/validate_items.py` use the shared package imports and strict model validation.
- [ ] 3.3 Update the GUI to display logical path separately from allocation prefix and address capacity.
- [ ] 3.4 Add deterministic free-range/headroom views and warnings for fragmented or nearly exhausted groups.
- [ ] 3.5 Preserve selection and dirty state across redraws and ensure all mutating GUI operations are tracked.

## 4. Tests and data review

- [x] 4.1 Add model tests for equivalent allocation-prefix spellings (`0:1:0` and `0:10`), complete item-ID packing with an appended payload, prefix containment, and capacity boundaries.
- [x] 4.2 Add tests for parent direct items plus child reservations without a synthetic `misc` group.
- [x] 4.3 Add tests for arbitrary logical depth with shared allocation pools.
- [ ] 4.4 Add CLI tests proving dry-run commands do not write and invalid input returns non-zero.
- [ ] 4.5 Draft and review the initial registry group manifest; do not merge `todo_items/items (1).csv` automatically.
- [ ] 4.6 Run Python tests, strict validation, and graphify update after implementation.
