## 1. Contract and threat scope

- [x] 1.1 Specify account registration/login, stable identity mapping, pre-auth denial, legacy profile non-migration, and trusted-LAN/VPN-only plaintext transport.
- [x] 1.2 Strictly validate the OpenSpec change and obtain user approval before implementation.

## 2. Auth library and persistence

- [x] 2.1 Add a reusable Go auth package with canonical username validation and Argon2id password hashing/verification.
- [x] 2.2 Add MetaDB account and account-to-player mapping schema; atomically allocate never-reused fresh numeric IDs above all existing legacy IDs.
- [x] 2.3 Keep old numeric profiles unmapped and disable all unauthenticated numeric-ID JSON profile reads/writes.
- [x] 2.4 Add bounded inputs, generic login failures, per-account/IP rate limits, secret-safe logging, and at most two concurrent KDF operations.

## 3. Gateway and client lifecycle

- [x] 3.1 Add a dedicated direct Gateway-to-MetaDB FlatBuffers auth listener on 127.0.0.1:5007 by default, with explicit private-interface override, 64 KiB frame cap and 3-second RPC deadline; do not use MessageRouter.
- [x] 3.2 Keep ctrl connections unauthenticated until success; apply a non-extendable 15-second accept-to-auth deadline and only then create/bind player session, send welcome, publish join, restore state, and pair bulk.
- [x] 3.3 Enforce one active session per account in a single authoritative Gateway; release reservations on disconnect/timeout/restart.
- [x] 3.4 Add client registration/login before entering gameplay networking; retain plaintext trusted-network-only scope and keep credentials out of logs and MessageRouter.

## 4. Verification

- [x] 4.1 Test registration uniqueness/atomicity, password validation/hash verification, stable mapping after restart, never-reused allocation above legacy rows including `MaxInt64`, legacy-row isolation, and same Argon2 verification work for unknown usernames and wrong passwords.
- [x] 4.2 Test Gateway pre-auth denial, valid/invalid login, identity spoof rejection, duplicate-session policy, timeout/disconnect cleanup, direct RPC limits and absence of pre-auth side effects; verify frames before reading FlatBuffers fields.
- [x] 4.3 Test client login failure/retry and authenticated bulk pairing; gate all gameplay sends until welcome and do not retry rejected credentials after reconnect.

## 5. Remember client credentials

- [x] 5.1 Add a Linux credential store using XDG_CONFIG_HOME or HOME fallback, plaintext JSON, restrictive directory/file permissions, safe parsing, and atomic replacement.
- [x] 5.2 Auto-login from a validated saved credential file on client startup; retain manual login fallback and save credentials only after successful AuthResult.
- [x] 5.3 Remove rejected saved credentials, preserve username for manual correction, and never log secrets.
- [x] 5.4 Add credential-store permission/path/malformed-file tests and auth-flow tests for success-only persistence, next-launch auto-login, and failure fallback; document plaintext-at-rest limits.
