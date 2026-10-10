## ADDED Requirements

### Requirement: Account Registration and Login
The system SHALL authenticate a player account before creating an active Gateway player session.

#### Scenario: Register a new account
- **GIVEN** a client submits a valid, previously unused normalized username and acceptable password over the trusted private network
- **WHEN** MetaDB creates the account
- **THEN** it SHALL store an immutable stable account identity and a password hash produced by a maintained password-hashing implementation
- **AND** it SHALL atomically allocate a fresh numeric `player_id` and bind that account to it
- **AND** it SHALL NOT associate the account with any pre-existing unmapped numeric player profile

#### Scenario: Login resolves stable player identity
- **GIVEN** a registered account
- **WHEN** the account owner logs in after a client reconnect or service restart
- **THEN** authentication SHALL resolve to the same stable account identity and mapped numeric `player_id`
- **AND** player inventory, position, quests, rewards, and cooldowns SHALL continue to use that numeric `player_id`

#### Scenario: Invalid credentials do not activate a player
- **GIVEN** a client submits an unknown username or incorrect password
- **WHEN** MetaDB verifies the login request
- **THEN** authentication SHALL fail with a generic credential error
- **AND** Gateway SHALL NOT send an authenticated player welcome or publish `player.joined`
- **AND** Gateway SHALL NOT restore player state or accept gameplay actions for that connection

#### Scenario: Duplicate account registration is atomic
- **GIVEN** a normalized username is already registered
- **WHEN** another registration uses that username
- **THEN** registration SHALL fail without creating an orphan `players` row or account mapping

#### Scenario: Credential and username bounds are enforced
- **GIVEN** a username or password is not valid UTF-8, the username after trimming leading/trailing ASCII space and horizontal tab and converting ASCII uppercase to lowercase does not match `[a-z0-9_]` or is not 3–32 bytes, or the password is not 8–128 bytes
- **WHEN** the auth library validates registration or login input
- **THEN** it SHALL reject the request before performing password hashing
- **AND** registration uniqueness and login lookup SHALL use the same canonical username
- **AND** it SHALL NOT log the username/password payload or password hash

#### Scenario: Password hashes use bounded Argon2id parameters
- **GIVEN** a password passes input validation
- **WHEN** the auth library stores its password hash
- **THEN** it SHALL use Argon2id v1.3 with time cost 3, memory 65536 KiB, parallelism 1, a cryptographically random 16-byte salt, and a 32-byte output
- **AND** the stored representation SHALL include a version and parameters
- **AND** verification SHALL reject encodings with time cost above 6, memory above 262144 KiB, parallelism above 4, salt outside 16–64 bytes, or output outside 32–64 bytes before allocating KDF memory
- **AND** no more than two Argon2id operations SHALL execute concurrently per MetaDB process

#### Scenario: Login errors do not enumerate accounts
- **GIVEN** a login names either an unknown username or an existing username with the wrong password
- **WHEN** MetaDB responds
- **THEN** both cases SHALL return the same externally visible invalid-credentials result

#### Scenario: Client authenticates over the control connection
- **GIVEN** the game client has connected to Gateway but has not authenticated
- **WHEN** the player submits registration or login from the pre-game ImGui screen
- **THEN** the client SHALL send the bounded auth request over the existing ctrl connection
- **AND** Gateway SHALL return a distinct generic AuthResult before sending SessionWelcome
- **AND** the client SHALL not send gameplay or bulk-pair messages until successful AuthResult and SessionWelcome
- **AND** the client SHALL retain credentials in process memory only as needed for reconnects
- **AND** after successful AuthResult it SHALL save credentials as specified by Remember Client Credentials
- **AND** the client SHALL authenticate again on every new Gateway session

### Requirement: Remember Client Credentials
The client SHALL support automatic sign-in on subsequent launches by saving credentials after successful authentication.

#### Scenario: Save credentials only after successful authentication
- **GIVEN** the player submits registration or login from the client
- **WHEN** Gateway returns successful AuthResult
- **THEN** the client SHALL save the canonical username and password to its per-user credential file
- **AND** it SHALL NOT write credentials when the request is submitted or when authentication fails
- **AND** it SHALL never log the credentials or the serialized file contents

#### Scenario: Automatically sign in on a later launch
- **GIVEN** the per-user credential file exists and passes safety validation
- **WHEN** the client connects to Gateway on a later launch
- **THEN** it SHALL submit a login request with those credentials automatically
- **AND** it SHALL still wait for successful AuthResult and SessionWelcome before gameplay or bulk pairing

#### Scenario: Fall back to manual login when saved credentials cannot be used
- **GIVEN** the credential file is missing, malformed, unsafe, inaccessible, or its saved credentials are rejected
- **WHEN** the client starts or attempts automatic login
- **THEN** it SHALL keep the login form available and allow manual credential entry
- **AND** for rejected saved credentials it SHALL remove the rejected password and may prefill the username
- **AND** it SHALL clear rejected credentials from in-memory reconnect state
- **AND** it SHALL not store a failed manual attempt

#### Scenario: Protect the plaintext client credential file
- **GIVEN** the client saves or loads credentials
- **WHEN** the per-user credential file is created, read, or replaced
- **THEN** it SHALL use the XDG config directory or `$HOME/.config` fallback
- **AND** the application directory SHALL be restricted to mode 0700 and the file to mode 0600
- **AND** writes SHALL replace the file atomically and reject unsafe symlinks, non-regular files, oversized or malformed content
- **AND** documentation SHALL state that the stored password is plaintext and can be read by the same OS user, root, malware, or backups

### Requirement: Gateway Authentication Gate
Gateway SHALL distinguish unauthenticated transport connections from authenticated player sessions.

#### Scenario: Gameplay is unavailable before authentication
- **GIVEN** a newly accepted ctrl connection has not authenticated
- **WHEN** it sends a gameplay action or attempts to pair a bulk connection
- **THEN** Gateway SHALL reject the action/pairing
- **AND** SHALL NOT publish player lifecycle events or request player inventory, quests, or position
- **AND** the pending ctrl connection SHALL be closed if authentication is not completed within 15 seconds

#### Scenario: Authenticated session binds server identity
- **GIVEN** MetaDB successfully authenticates an account and resolves its numeric `player_id`
- **WHEN** Gateway activates the connection
- **THEN** the Gateway session SHALL bind that server-resolved ID and stable account identity
- **AND** all later claimed player IDs SHALL be checked against the bound numeric ID
- **AND** the bulk socket SHALL pair only to that session's ephemeral session ID

#### Scenario: Concurrent login policy
- **GIVEN** an account already has an active Gateway session
- **WHEN** the same account attempts another login
- **THEN** Gateway SHALL reject the additional login without disturbing the active session
- **AND** a disconnected or expired session SHALL release its active-account reservation
- **AND** Gateway restart SHALL clear in-memory reservations so the account can log in again
- **AND** the deployment SHALL use exactly one authoritative Gateway instance

### Requirement: Trusted-Network Credential Boundary
This initial account-auth deployment SHALL operate only on a trusted private LAN/VPN using plaintext transport. Registration/login RPC traffic between Gateway and MetaDB SHALL use a dedicated direct FlatBuffers endpoint, bound to loopback by default or to an explicitly configured private interface, and SHALL NOT traverse MessageRouter.

#### Scenario: Credentials remain within trusted deployment
- **GIVEN** account registration or login traffic is sent over the configured plaintext connection
- **WHEN** the feature is deployed
- **THEN** documentation SHALL warn that credentials, tokens, and session identifiers are observable and replayable on untrusted networks
- **AND** the service SHALL NOT be represented as safe for public/hostile networks
- **AND** credentials, password hashes, and tokens SHALL NOT be published through MessageRouter or written to logs
- **AND** the dedicated auth endpoint SHALL enforce bounded frames, request deadlines, and bounded concurrent password-hash work
- **AND** unauthenticated numeric-ID MetaDB JSON/FlatBuffers listeners SHALL bind to loopback by default and require explicit private-interface configuration for remote access

#### Scenario: Legacy numeric profiles are not claimed
- **GIVEN** a pre-existing MetaDB player row has no account mapping
- **WHEN** a new account registers or logs in
- **THEN** the account SHALL NOT be assigned that row automatically based on a numeric ID, nickname, or username

#### Scenario: Account registration allocates a never-reused player ID
- **GIVEN** MetaDB contains legacy player rows and a persisted player-ID high-water mark
- **WHEN** a new account registration transaction allocates its gameplay profile
- **THEN** the allocated `player_id` SHALL be greater than the high-water mark and every existing `players.id`
- **AND** account, player row, and mapping SHALL commit or roll back atomically
- **AND** a player ID allocated to an account SHALL NOT be reused after row deletion or MetaDB restart
- **AND** signed 64-bit ID exhaustion SHALL fail registration without partial rows

#### Scenario: Legacy unauthenticated numeric profile operations are disabled
- **GIVEN** a client invokes a legacy JSON `login`, `logout`, `update_slot`, `delete_slot`, or `save_position` action with a caller-supplied numeric `player_id`
- **WHEN** MetaDB handles the request
- **THEN** it SHALL reject the request without returning or changing that player's profile data
- **AND** authenticated account login SHALL be the only supported client profile access path
