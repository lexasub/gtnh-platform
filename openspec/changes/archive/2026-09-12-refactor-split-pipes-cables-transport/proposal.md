# Change: Split pipes-cables-transport spec into focused capabilities

## Why
The `pipes-cables-transport` spec accumulated requirements from several independent features and
became a grab-bag: navigation cost is high and the 10-minute understandability rule
is violated. This change extracts the self-contained, already-implemented
sub-capabilities into dedicated specs without changing any normative content.

## What Changes
- Extract ready sub-capabilities from `pipes-cables-transport` into dedicated specs: pipe-fluid-overlay, pipe-wrench-guidance, pipe-face-masking, pipe-steam-transport.

## Impact
- Affected specs: `pipes-cables-transport` (REMOVED), `pipe-fluid-overlay` (ADDED), `pipe-wrench-guidance` (ADDED), `pipe-face-masking` (ADDED), `pipe-steam-transport` (ADDED).
- Affected code: none (spec-only decomposition).
- **Non-breaking**: requirement text moves verbatim; no behavior change.
