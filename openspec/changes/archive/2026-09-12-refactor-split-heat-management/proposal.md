# Change: Split heat-management spec into focused capabilities

## Why
The `heat-management` spec accumulated requirements from several independent features and
became a grab-bag: navigation cost is high and the 10-minute understandability rule
is violated. This change extracts the self-contained, already-implemented
sub-capabilities into dedicated specs without changing any normative content.

## What Changes
- Extract ready sub-capabilities from `heat-management` into dedicated specs: boiler-steam-production.

## Impact
- Affected specs: `heat-management` (REMOVED), `boiler-steam-production` (ADDED).
- Affected code: none (spec-only decomposition).
- **Non-breaking**: requirement text moves verbatim; no behavior change.
