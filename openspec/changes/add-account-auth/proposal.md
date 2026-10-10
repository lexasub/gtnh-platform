# Change: Add account registration and stable player identity

## Why

The current Gateway assigns `player_id` from an in-memory session counter. It is therefore not a durable user identity across disconnects or service restarts, while MetaDB persists player state under numeric `player_id`. Players need a simple registration/login flow that resolves a stable account to a persistent gameplay profile.

## What Changes

- Add custom account registration and login for the trusted LAN/VPN deployment.
- After successful authentication, save the username and password in a private per-user client config file and attempt login from that file on later launches; use XDG config storage with restrictive permissions and atomic replacement.
- Give every new account an immutable stable account identity and map it to a persistent numeric `player_id` used by existing gameplay services.
- Authenticate before Gateway creates/activates the player session, announces `player.joined`, restores state, or accepts gameplay actions.
- Keep auth policy separate from gameplay: reusable Go auth package for credential policy; MetaDB owns persistence; Gateway owns transport/session binding.
- Use Argon2id from the maintained `golang.org/x/crypto/argon2` package and a private, dedicated MetaDB FlatBuffers auth listener (loopback by default), not the public legacy JSON listener.
- Apply bounded registration/login frames, input lengths, password-hash concurrency, pre-auth socket deadlines, and login rate limits.
- Disable all legacy unauthenticated JSON operations keyed only by caller-supplied numeric `player_id`, so they cannot read or mutate player profiles outside account authentication.
- Do not automatically associate existing numeric player profiles with newly registered accounts.

## Security Scope

This initial deployment explicitly accepts plaintext transport on a trusted LAN/VPN. Credentials, session values, and traffic are observable and tamperable on untrusted networks. The feature SHALL be documented as trusted-network-only and SHALL NOT be represented as safe for public or hostile networks. Server-side passwords SHALL be hashed at rest with a maintained password-hashing implementation, never logged, and never sent through MessageRouter. The client credential file SHALL contain the password in plaintext, be restricted to the current OS user (directory mode 0700, file mode 0600), and SHALL NOT be described as encrypted; it remains readable by that user, root, malware running as that user, and backups. TLS is outside this initial scope and is a prerequisite before untrusted-network deployment.

## Impact

- Affected capabilities: `multiplayer-sync`; new account-auth requirements.
- Affected code: reusable Go auth package, MetaDB account schema/API, Gateway authentication lifecycle, FlatBuffers/wire message constants, game-client login and local credential store, integration tests and deployment docs.
- Existing gameplay data remains keyed by numeric `player_id`; legacy numeric profiles stay unclaimed unless separately migrated by an explicit future process.
