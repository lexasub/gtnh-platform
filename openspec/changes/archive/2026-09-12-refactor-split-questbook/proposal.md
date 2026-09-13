# Change: Split questbook spec into focused capabilities

## Why
The `questbook` spec accumulated requirements from several independent features and
became a grab-bag: navigation cost is high and the 10-minute understandability rule
is violated. This change extracts the self-contained, already-implemented
sub-capabilities into dedicated specs without changing any normative content.

## What Changes
- Extract ready sub-capabilities from `questbook` into dedicated specs: questbook-exchange, questbook-quest-data, questbook-icons-lock.

## Impact
- Affected specs: `questbook` (REMOVED), `questbook-exchange` (ADDED), `questbook-quest-data` (ADDED), `questbook-icons-lock` (ADDED).
- Affected code: none (spec-only decomposition).
- **Non-breaking**: requirement text moves verbatim; no behavior change.
