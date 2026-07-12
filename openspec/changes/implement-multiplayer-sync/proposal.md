# Change: Implement Multiplayer Sync

## Why
Userflow 05 describes multiplayer sync (block changes broadcast to all clients, player disconnect/reconnect, state restoration). Core pub/sub exists but formal multiplayer sync flow needs documentation and gap-filling.

## What Changes
- Formalize block change broadcast protocol (all clients receive updates)
- Formalize player disconnect/reconnect flow (state cleanup, restoration)
- Verify cross-client block sync works end-to-end
- Document service communication patterns (pub/sub, RPC, chained events)

## Impact
- Affected specs: multiplayer-sync (new)
- Affected code: gateway (client broadcast), simulation_core (player state), message_router (topics)
