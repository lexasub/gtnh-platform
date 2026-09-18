## 1. Additive scanner

- [ ] 1.1 Replace hardcoded full rebuild with read/validate/plan/write phases.
- [ ] 1.2 Preserve existing CSV rows/comments/order and existing pack pixels.
- [ ] 1.3 Discover raw categories/assets dynamically and allocate deterministic append-only IDs.
- [ ] 1.4 Add dry-run default and explicit `--apply` writing mode.
- [ ] 1.5 Add optional explicit metadata for item/block/merge semantics.

## 2. Validation and tests

- [ ] 2.1 Test no-op and idempotent scans.
- [ ] 2.2 Test preservation of manual rows and pack pixels.
- [ ] 2.3 Test exact, ambiguous, missing, malformed, duplicate, and overflow cases.
- [ ] 2.4 Validate texture, face, item, and merge references.

## 3. Documentation

- [ ] 3.1 Update texture atlas documentation with canonical manual ownership.
- [ ] 3.2 Validate the OpenSpec change strictly.
