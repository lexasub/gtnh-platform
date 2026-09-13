# Change: Split player-interaction spec into focused capabilities

## Why
The `player-interaction` spec accumulated requirements from several independent features and
became a grab-bag: navigation cost is high and the 10-minute understandability rule
is violated. This change extracts the self-contained, already-implemented
sub-capabilities into dedicated specs without changing any normative content.

## What Changes
- Extract ready sub-capabilities from `player-interaction` into dedicated specs: inventory-interaction.

## Impact
- Affected specs: `player-interaction` (REMOVED), `inventory-interaction` (ADDED).
- Affected code: none (spec-only decomposition).
- **Non-breaking**: requirement text moves verbatim; no behavior change.
