## 1. Additive scanner

- [x] 1.1 Replace hardcoded full rebuild with read/validate/plan/write phases.
- [x] 1.2 Preserve existing CSV rows/comments/order and existing pack pixels.
- [x] 1.3 Discover raw categories/assets dynamically and allocate deterministic append-only IDs.
- [x] 1.4 Add dry-run default and explicit `--apply` writing mode.
- [x] 1.5 Add optional explicit metadata for item/block/merge semantics.

## 2. Validation and tests

- [x] 2.1 Test no-op and idempotent scans.
- [x] 2.2 Test preservation of manual rows and pack pixels.
- [x] 2.3 Test exact, ambiguous, missing, malformed, duplicate, and overflow cases.
- [x] 2.4 Validate texture, face, item, and merge references.

## 3. Documentation

- [x] 3.1 Update texture atlas documentation with canonical manual ownership.
- [x] 3.2 Validate the OpenSpec change strictly.
