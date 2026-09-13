## Context

`ItemId::pack()` accepts an item ID whose segments before the final colon are binary bits, ignoring the colon separators, and treats the final colon-separated segment as the decimal payload. An allocation prefix is a different grammar: it contains only prefix segments and has no payload segment. Therefore allocation prefixes `0:1:0` and `0:10` both canonicalize to bits `010`, while passing either string directly to `ItemId::pack()` would be wrong because `pack()` would interpret its final segment as payload. Items under that range are written with an explicit payload, for example `0:1:0:7` or `0:10:7`, which pack to the same ID. The current editor groups by unnormalized textual prefixes and uses subtree item counts as capacity, which does not describe the packed-ID namespace.

The registry has two independent concerns:

1. **Logical organization** — an arbitrarily deep path used for browsing, labels, search, and documentation.
2. **Address allocation** — a canonical binary prefix range used to allocate and validate packed item IDs.

## Goals / Non-Goals

- Goals:
  - Represent logical groups independently from allocation ranges.
  - Use the same prefix-bit and capacity rules as runtime `ItemId::pack()`, while keeping allocation-prefix parsing separate from complete item-ID parsing.
  - Support variable-length prefix allocation: short prefixes for large groups and long prefixes for small groups.
  - Account for child reservations and direct items without requiring a synthetic `misc` group.
  - Preserve all existing IDs during validation, merge, and allocation.
  - Make capacity, free ranges, and growth headroom visible in GUI and CLI.
- Non-Goals:
  - Changing the runtime packed-ID format or widening `uint16_t`.
  - Automatically renumbering existing items.
  - Migrating legacy numeric recipe IDs, machine IDs, ore IDs, or fluid IDs.
  - Making every logical subgroup an allocation subgroup.

## Data Model

A group manifest contains records with:

```yaml
version: 1
groups:
  - path: base/materials
    allocation_prefix: "0:110"
    reserve_ratio: 0.25
    item_ids: ["0:110:1", "0:110:2"]
  - path: base/materials/plates
    allocation_prefix: "0:110:10"
    reserve_ratio: 0.25
    item_ids: []
  - path: base/materials/plates/copper
    allocation_prefix: null
    item_ids: []
```

- `path` is a normalized logical path. Its depth is not constrained by the packed format.
- `allocation_prefix` is optional. It is parsed as an allocation-prefix string, not as a complete item ID: every segment contributes binary prefix bits and there is no payload segment. A null prefix means the logical group shares its nearest allocated ancestor's pool and does not reserve a new range.
- A non-null allocation prefix is canonicalized by concatenating its segments, requiring only `0` and `1`, and requiring 1–15 bits. To construct an item ID in that range, append a decimal payload segment before calling the runtime packing rule.
- `item_ids` is a required list of exact existing hierarchical IDs assigned to the logical group; `item_ids: []` explicitly means empty membership. Every valid item in `items.csv` must appear exactly once across the manifest (or in an explicit unassigned/root group). Allocated groups additionally validate membership by canonical range containment; logical-only groups always require explicit membership.
- The root allocation namespace is the complete 16-bit space. Configured top-level namespaces such as `0`, `10`, `110`, `1110`, and `1111` are explicit data decisions; comments and category labels do not create namespaces. The initial manifest must be checked against the actual packed IDs in `items.csv`, including legacy/base rows that may not match comment labels.
- Existing `items.csv` IDs remain the source of item definitions. A group manifest must not redefine item names, stack sizes, or metadata.

## Allocation and Capacity Rules

For a canonical allocation prefix of length `L`, the range capacity is:

```text
2^(16-L)
```

The payload range is `0..2^(16-L)-1`.

Allocation groups form a prefix tree:

- A child allocation prefix must begin with its parent's canonical bits.
- Sibling allocation ranges must not overlap.
- Equal canonical prefixes are one allocation range, even when their textual separators differ; they may be aliases/logical views but cannot be independent pools.
- A logical child may omit `allocation_prefix` and share its nearest allocated ancestor.
- Direct items may remain in any free address portion of their nearest allocation ancestor, provided they do not fall inside a child reservation.
- Existing direct IDs outside child ranges must not be moved to make a child fit.
- A new child range is sized from requested items, expected growth, and headroom, then rounded to an aligned power-of-two prefix block. Failure to find a non-overlapping block is a validation/allocation error, not a silent relocation.

Capacity reporting must expose at least:

- total address capacity;
- addresses reserved by direct allocation children;
- direct IDs used;
- descendant IDs used;
- free ranges available to direct items or future children;
- configured and absolute growth headroom;
- logical item count (reported separately from address capacity).

## Validation Rules

Strict validation fails for:

- non-binary prefix characters or more than 15 canonical prefix bits;
- payload outside the range permitted by the prefix;
- duplicate packed IDs, including textual aliases that canonicalize to the same value;
- duplicate independent allocation prefixes;
- overlapping sibling allocation ranges;
- allocation child prefixes outside their declared logical parent's range;
- direct items located inside a child allocation range unless that relationship is explicitly declared;
- group manifest paths that are duplicated or have inconsistent parents;
- a requested allocation that would require moving an existing item.

Comments and display labels never define namespaces or alter canonical prefixes.

## CLI / GUI Surface

The shared model should support these operations:

- `validate`: strict item and manifest validation with non-zero exit on errors;
- `stats`: range capacity, reservations, used/free ranges, and headroom;
- `allocate`: dry-run or explicit write of new IDs inside a selected allocation pool;
- `merge --dry-run`: classify aliases and conflicts while preserving current IDs;
- GUI tree: show logical path and allocation prefix/range as separate fields, with address-capacity bars and logical item counts.

All write operations require explicit output or confirmation. Dry-run output must be deterministic and must not modify the source files.

## Risks / Trade-offs

- A separate manifest adds one file and requires keeping logical paths coherent. Validation will treat malformed or stale manifest entries as hard errors.
- Parent ranges can become fragmented after child reservations. The allocator must operate on free intervals, not only a used-count counter.
- Logical-only groups can be visually deep while sharing an allocation pool; the UI must make that sharing explicit to avoid misleading users.
- Existing data may not have enough metadata to infer intended logical parents. The migration starts with explicit manifest entries and rejects ambiguous inference.

## Migration Plan

1. Add model-level canonical prefix and range helpers without modifying existing `items.csv` IDs.
2. Create a manifest for currently agreed top-level and subcategory ranges; review it before enabling writes.
3. Run strict validation and capacity stats against the current registry.
4. Add GUI/CLI read-only views and dry-run allocation/merge.
5. Enable writes only after manifest review and tests pass.

Rollback is deleting the manifest/tooling change; existing `items.csv` IDs and runtime behavior remain unchanged.

## Open Questions

- Which exact logical paths and allocation prefixes are authoritative for the expanded draft catalog?
- Should manifest serialization remain YAML, or use a CSV companion compatible with the existing registry workflow?
- What default growth headroom should be used when a group has no explicit policy?
