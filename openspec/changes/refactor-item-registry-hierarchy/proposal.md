# Change: Separate logical item groups from packed-ID allocation ranges

## Why

The item registry editor currently treats the textual prefix in `items.csv` as both a visual tree and an allocation boundary. That makes capacity reporting misleading and makes differently segmented allocation-prefix spellings such as `0:1:0` and `0:10` look like different groups even though both canonicalize to the same binary prefix (`010`). An allocation prefix is not a complete item ID: `ItemId::pack()` interprets the final colon-separated segment as a decimal payload, so allocation-prefix parsing must be separate and an item payload must be appended before runtime packing. The editor also cannot represent deep logical categorization without a separate group-to-item membership model.

The registry needs a variable-depth prefix allocation model: large categories receive short prefixes, smaller categories may receive longer prefixes, and logical nesting may be deeper than the packed allocation tree when no new ID range is required.

## What Changes

- Add an explicit group manifest for logical paths and optional packed-ID allocation prefixes.
- Canonicalize allocation prefixes by concatenating binary prefix segments with a dedicated allocation-prefix parser; use `ItemId::pack(prefix + ":" + payload)` only for complete item IDs.
- Model allocation groups as nested, non-overlapping variable-length prefix ranges with a maximum of 15 prefix bits.
- Permit direct items in a parent range outside ranges reserved by child groups; an artificial `misc` group is not required.
- Define capacity and free-space accounting from address ranges, direct IDs, child reservations, and configurable growth headroom.
- Add strict validation rules for malformed prefixes, payload overflow, duplicate canonical ranges, overlapping sibling ranges, and invalid logical/allocation relationships.
- Extend the item registry editor/CLI design to show logical hierarchy and allocation capacity separately, and to allocate new IDs without moving existing IDs.
- Preserve `data/registry/items.csv` as the canonical item catalog; the manifest describes organization and allocation metadata rather than duplicating item definitions.

## Impact

- Affected specs: `item-registry-hierarchy` (new capability), with compatibility to the existing `architecture` and `recipe-id-format` requirements.
- Affected code: `tools/editor_model.py`, `tools/item_registry_editor.py`, `tools/validate_items.py`, a new group-manifest data file under `data/registry/`, and tests for parsing, canonicalization, range accounting, and allocation.
- Existing item IDs remain stable. No automatic semantic migration of legacy numeric IDs or recipe references is included.
- This change is a design and tooling boundary; it does not alter the runtime `uint16_t` wire representation.
