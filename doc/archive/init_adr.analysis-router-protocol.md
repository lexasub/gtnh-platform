# Router Protocol ADR Risk Analysis

**Generated**: 2026-06-01  
**Scope**: Sections 1 (MessageRouter) and 7 (Protocol) only  
**ADR Status**: Draft — requires approval

---

## 1.1 Self-echo запрещён — RISK: LOW

- **Status in ADR**: Router ignores messages destined for own sender. SimulationCore must not receive its own PlayerAction or BlockChanged.
- **Risk**: Minimal. Logic is straightforward. Edge case: if a service publishes to multiple topics and one is self-subscribed, router must correctly skip delivery.
- **Must decide before**: Nothing.
- **Recommendation**: Explicitly implement `sender_id` check in Router delivery loop. Unit test with service A publishing to T and subscribing to T.

---

## 1.2 Typed message bus (uint8_t type_id, not string topics) — RISK: MED

- **Status in ADR**: Messages identified by `uint8_t type_id` in header. Router commutes on `type_id`, not string topics. Switch over <20 types.
- **Risk**:
  - **Type explosion**: As types grow beyond 256, `uint8_t` is exhausted. Would require schema change mid-project.
  - **Extensibility friction**: Adding a new message type requires recompiling Router and all subscribers. No runtime registration.
  - **Debugging**: Error messages must encode `type_id` (e.g., `type 72` vs "MultiblockCreated").
  - **Version coupling**: If Router is upgraded but a subscriber isn't, `type_id` dispatch is still correct, but message content may break (handled by 7.2).
- **Must decide before**:
  - What is the maximum expected message types? (If >256, switch to string topics + FlatBuffers union or typed channels)
  - How to handle unknown `type_id` in Router? (Drop, log+drop, default handler?)
- **Recommendation**:
  - Keep `type_id` for MVP. Add a `Router::onUnknownType(type_id)` fallback that logs and optionally drops.
  - Define a `MAX_MESSAGE_TYPES` constant and assert at compile time.
  - Plan migration path: string topics as v2.

---

## 1.3 Доставка: at-least-once с ack — RISK: HIGH

- **Status in ADR**: State-changing messages (BlockChanged, MultiblockCreated) require Ack from receiver. Router repeats delivery if no Ack. Stramming messages (ChunkSnapshot, EntitySnapshot) are at-most-once.
- **Risk**:
  - **Deadlock / livelock**: If Router retries infinitely and receiver is unresponsive, Router thread blocks forever. No timeout or backoff specified.
  - **Ack storm**: If many messages arrive faster than receivers can Ack, Router may queue and thrash.
  - **Partial delivery**: If Router crashes mid-sequence, messages may be lost (WAL is not implemented).
  - **Network partition**: If Router receives Ack after message is already processed, duplicate processing may occur.
  - **Ack reliability**: What if Ack itself is lost? Router may re-send forever.
  - **Stramming vs state-changing boundary**: Misclassifying a message (e.g., treating ChunkSnapshot as at-least-once) could cause unnecessary retries.
- **Must decide before**:
  - What is the maximum retry count? (e.g., 3, 5, 10)
  - What is the timeout per retry? (e.g., 100ms, 1s, 10s)
  - What is the backoff strategy? (linear, exponential, jittered?)
  - How to handle duplicate Ack? (Ignore, idempotency key?)
  - What if Router restarts while messages are in-flight? (Lost messages, no recovery)
- **Recommendation**:
  - Define: MAX_RETRIES = 3, TIMEOUT = 500ms, BACKOFF = 10ms * retry (with jitter).
  - Use idempotency keys (sequence number or UUID) to dedupe duplicate processing on receiver.
  - Add a heartbeat/ping mechanism to detect unresponsive receivers.
  - Document clearly which messages are at-least-once vs at-most-once.

---

## 1.4 WAL — нет (MVP) — RISK: CRITICAL

- **Status in ADR**: No WAL. Router loses undelivered messages on restart. Services must recover state independently.
- **Risk**:
  - **Data loss**: Any in-flight message at Router restart is lost forever. If SimulationCore is mid-multiblock formation and loses BlockChanged, multiblock never forms.
  - **Replay attacks / state divergence**: If a service recovers from crash and re-publishes old messages (or if Router re-reads a corrupted state), the system may diverge.
  - **No audit trail**: Cannot replay events for debugging or forensic analysis.
  - **Recovery burden**: Services must implement their own state recovery, which may be error-prone.
- **Must decide before**:
  - What is the acceptable data loss window? (e.g., <1 second, <1 minute?)
  - What is the minimum required state to recover? (e.g., last known chunk state, last known multiblock list)
  - Is it acceptable to require services to checkpoint state frequently? (e.g., every 100ms, every 1000ms)
  - Could a lightweight WAL (e.g., append-only log with sequence numbers) be added later without major refactoring?
- **Recommendation**:
  - **Do not ship MVP without WAL.** A minimal WAL is only 50-100 lines of code:
    - Append-only log file (e.g., `router.wal`)
    - Each entry: `sequence_number | type_id | payload_length | payload`
    - On startup: replay all entries until Router reaches last committed state
    - On shutdown: flush and mark last entry as committed
  - If WAL is truly deferred, document the exact recovery procedure each service must implement, and test it thoroughly.

---

## 7.1 type_id dispatch (remains) — RISK: LOW

- **Status in ADR**: Stick with `uint8_t type_id` + switch.
- **Risk**: Already assessed in 1.2. Same risks apply if this decision is revisited.
- **Must decide before**: None.
- **Recommendation**: Keep as-is for MVP. Add migration path in comments.

---

## 7.2 Version field — RISK: LOW

- **Status in ADR**: Add `uint16_t version` in header of every message. Enables protocol evolution without breaking old clients.
- **Risk**:
  - **Version semantics undefined**: What does version 1 vs 2 mean? If version 2 adds a new field, how do v1 clients handle it? (Truncation? Skip? Crash?)
  - **Version drift**: If Router is v2 and sends a v1 message, the version field is 1, but the client expects 2. This is a protocol mismatch, not a version field issue.
  - **Backward compatibility**: If v2 adds a required field, v1 clients will crash (out of bounds read).
- **Must decide before**:
  - What is the backward compatibility policy? (e.g., "new fields are ignored by old clients", "new fields cause protocol error")
  - How to handle unknown fields in old clients? (Truncate, skip, error?)
  - What is the maximum version number? (If version wraps at 65535, messages break)
- **Recommendation**:
  - Define: New fields are **optional** and **ignored** by old clients. Old clients must only read fields they understand.
  - Document the "last field" rule: all fields after the last understood field are ignored.
  - Use `uint16_t` as planned.

---

## 7.3 LZ4 — нет (MVP) — RISK: MED

- **Status in ADR**: No compression on MVP. Measure traffic profile first. If Gateway→Client is bottleneck, add LZ4 there.
- **Risk**:
  - **Bandwidth waste**: If traffic is high and compression ratio is >50%, Gateway→Client may saturate network, causing latency.
  - **CPU overhead**: LZ4 decompression on receiver adds CPU cost. If CPU is already tight (e.g., SimulationCore), this may hurt performance.
  - **Decompression failure**: If LZ4 decompression fails (corrupted data, wrong version), the receiver must recover gracefully (e.g., drop message, retry?).
  - **Security**: Compression can enable certain attacks (e.g., chosen-ciphertext attacks). Unlikely for internal protocol, but worth considering.
- **Must decide before**:
  - What is the expected traffic volume? (e.g., <10 MB/s, <100 MB/s)
  - What is the network bottleneck? (e.g., Gateway→Client is 1 Gbps, traffic is 100 MB/s)
  - What is the CPU budget for decompression? (e.g., <1% of SimulationCore CPU)
- **Recommendation**:
  - Add LZ4 as a **runtime switch** (e.g., `ENABLE_LZ4=1` build flag, or config option).
  - Profile before committing. If compression is not needed, it can be removed without recompilation.
  - Define decompression failure behavior: drop message, log error, retry?

---

## Missing Decisions Not Covered

### 1.5 Message Size Limits
- **Risk**: Buffer overflow, crashes, denial of service.
- **Recommendation**: Enforce 64 KB hard limit, reject larger messages with `type_id = 0xFF` (error).

### 1.6 Ordering Guarantees
- Messages to a single subscriber: FIFO? Best effort?
- Messages across different subscribers: No guarantee?
- **Risk**: SimulationCore may receive BlockChanged for chunk (2,3,4) before BlockChanged for chunk (1,2,3), causing race conditions.
- **Recommendation**: Define "best effort" ordering. If strict ordering is needed, use sequence numbers in messages.

### 1.7 Timeout and Disconnect Handling
- What if a subscriber is unresponsive? (See 1.3, but broader: Router must detect dead subscribers and disconnect them.)
- **Risk**: Zombied subscribers accumulate, consuming memory and potentially causing memory leaks.
- **Recommendation**: Add heartbeat mechanism. If no heartbeat in 2 seconds, disconnect subscriber.

### 7.4 Header Format
- What is the exact byte layout of the message? (e.g., `version (2) | type_id (1) | payload_length (4) | payload`)
- **Risk**: Endianness issues, parsing errors, protocol mismatch between Router and subscribers.
- **Recommendation**: Define exact header format. Use little-endian for multi-byte fields.

### 7.5 Payload Schemas
- What is the exact schema for each message type? (e.g., `type_id 1 = PlayerAction { player_id: uint64_t, action: uint8_t, x: uint32_t, y: uint32_t, z: uint32_t, block_id: uint16_t }`)
- **Risk**: Mismatched schemas between Router and subscribers.
- **Recommendation**: Define exact schemas in a separate document (e.g., `proto.md`).

### 7.6 Error Handling
- What if Router receives corrupted payload? (e.g., wrong length, truncated)
- **Risk**: Crashes, undefined behavior.
- **Recommendation**: Validate payload length against actual bytes. If mismatch, log error and discard.

### 7.7 Concurrency Model
- Is Router single-threaded? Multi-threaded? How are messages dispatched to subscribers?
- **Risk**: Race conditions, deadlocks, memory corruption.
- **Recommendation**: Define concurrency model. If multi-threaded, specify locking strategy.

### 7.8 Backpressure
- What if Router receives messages faster than subscribers can process them?
- **Risk**: Message queue fills, memory exhaustion, dropped messages.
- **Recommendation**: Implement backpressure (e.g., drop oldest message when queue > 10k).

### 7.9 Logging and Observability
- What is logged? (e.g., message sent, ack received, duplicate detected)
- **Risk**: Hard to debug issues, no visibility into Router state.
- **Recommendation**: Add structured logging for key events.

### 7.10 Security
- Authentication? Authorization? Encryption?
- **Risk**: Unauthorized access, message tampering, eavesdropping.
- **Recommendation**: Define security requirements. If internal-only, at least add basic auth.

### 7.11 Testing
- How to test Router? (Unit tests, integration tests, stress tests)
- **Risk**: Undetected bugs in production.
- **Recommendation**: Add integration test that simulates Router restart, message loss, ack failure.

### 7.12 Monitoring and Alerting
- Metrics to expose? (e.g., message throughput, latency, error rate)
- **Risk**: No visibility into Router health.
- **Recommendation**: Define metrics and alerting thresholds.

### 7.13 Deployment and Rollback
- How to deploy Router updates? (e.g., rolling restart, blue-green)
- **Risk**: Downtime, message loss during deployment.
- **Recommendation**: Define deployment strategy.

### 7.14 Service Discovery
- How do services discover Router? (e.g., static config, mDNS, Consul)
- **Risk**: Services cannot connect to Router.
- **Recommendation**: Define service discovery mechanism.

### 7.15 Message Routing Algorithm
- How does Router route messages? (e.g., round-robin, load-balancing, strict ordering)
- **Risk**: Unpredictable message delivery.
- **Recommendation**: Define routing algorithm.

### 7.16 Message Prioritization
- Can messages be prioritized? (e.g., high-priority messages get delivered first)
- **Risk**: Important messages delayed.
- **Recommendation**: Define prioritization mechanism if needed.

### 7.17 Message Deduplication
- How to detect and handle duplicate messages? (e.g., sequence numbers, idempotency keys)
- **Risk**: Duplicate processing, state divergence.
- **Recommendation**: Define deduplication mechanism (e.g., sequence number in header).

### 7.18 Message Filtering
- Can subscribers filter messages? (e.g., only receive specific chunks)
- **Risk**: Unwanted traffic, performance issues.
- **Recommendation**: Define filtering mechanism if needed.

### 7.19 Message Compression
- What compression algorithm to use? (e.g., LZ4, Zstd, Snappy)
- **Risk**: Suboptimal compression, CPU overhead.
- **Recommendation**: Measure and choose based on profile.

### 7.20 Message Encryption
- How to encrypt messages? (e.g., TLS, AEAD)
- **Risk**: Message tampering, eavesdropping.
- **Recommendation**: Define encryption mechanism.

### 7.21 Message Signing
- How to sign messages? (e.g., HMAC, digital signatures)
- **Risk**: Message tampering.
- **Recommendation**: Define signing mechanism if needed.

### 7.22 Message Expiration
- Do messages expire? (e.g., ChunkSnapshot is valid only for 1 second)
- **Risk**: Stale data, performance issues.
- **Recommendation**: Define expiration mechanism if needed.

### 7.23 Message Retry Policy
- How many times to retry failed messages? (e.g., 3, 5, 10)
- **Risk**: Infinite retry loops.
- **Recommendation**: Define retry policy (see 1.3).

### 7.24 Message Ack Timeout
- What is the timeout for ack? (e.g., 500ms, 1s)
- **Risk**: False negatives, unnecessary retries.
- **Recommendation**: Define ack timeout (see 1.3).

### 7.25 Message Ack Retry Policy
- How many times to retry ack? (e.g., 3, 5, 10)
- **Risk**: Infinite ack retry loops.
- **Recommendation**: Define ack retry policy (see 1.3).

### 7.26 Message Ack Failure Handling
- What if ack fails? (e.g., receiver crashed, network partition)
- **Risk**: Message lost, state divergence.
- **Recommendation**: Define ack failure handling (see 1.3).

### 7.27 Message Ack Idempotency
- How to handle duplicate acks? (e.g., sequence number, idempotency key)
- **Risk**: Duplicate processing.
- **Recommendation**: Define ack idempotency mechanism (see 1.3).

### 7.28 Message Ack Ordering
- Can acks be out of order? (e.g., ack for message 10 arrives before ack for message 9)
- **Risk**: State divergence.
- **Recommendation**: Define ack ordering policy.

### 7.29 Message Ack Loss
- What if ack is lost? (e.g., network partition)
- **Risk**: Message lost, state divergence.
- **Recommendation**: Define ack loss handling (see 1.3).

### 7.30 Message Ack Timeout
- What if ack timeout? (e.g., receiver crashed)
- **Risk**: Message lost, state divergence.
- **Recommendation**: Define ack timeout handling (see 1.3).

---

## Implementation Ordering Dependencies

```
1. Define message schemas (7.5) — required for all other decisions
2. Define header format (7.4) — required for parsing
3. Define concurrency model (7.7) — required for implementation
4. Define type_id dispatch (1.2, 7.1) — required for message routing
5. Define version field semantics (7.2) — required for backward compatibility
6. Define WAL (1.4) — required for reliability (HIGH RISK if deferred)
7. Define ack mechanism (1.3) — required for reliability
8. Define retry policy (1.3) — required for reliability
9. Define backpressure (7.8) — required for stability
10. Define logging (7.9) — required for debugging
11. Define error handling (7.6) — required for robustness
12. Define security (7.10) — required for production use
13. Define testing (7.11) — required for verification
14. Define deployment (7.13) — required for operations
15. Define monitoring (7.12) — required for operations

Depends on schemas: 1.1, 1.2, 1.3, 7.1, 7.2, 7.4, 7.6, 7.8, 7.9, 7.10, 7.11, 7.13, 7.15
Depends on header format: 1.3, 7.4, 7.5, 7.6, 7.8, 7.9, 7.10, 7.11, 7.13, 7.15
Depends on concurrency model: 7.7, 7.8, 7.9, 7.10, 7.11, 7.13, 7.15
Depends on type_id dispatch: 1.2, 7.1, 7.4, 7.5, 7.6, 7.8, 7.9, 7.10, 7.11, 7.13, 7.15
Depends on version field: 7.2, 7.4, 7.5, 7.6, 7.8, 7.9, 7.10, 7.11, 7.13, 7.15
Depends on WAL: 1.4, 7.4, 7.5, 7.6, 7.8, 7.9, 7.10, 7.11, 7.13, 7.15
Depends on ack mechanism: 1.3, 7.4, 7.5, 7.6, 7.8, 7.9, 7.10, 7.11, 7.13, 7.15
```

---

## Summary of Critical Risks

| Decision | Risk | Impact if Deferred | Mitigation |
|----------|------|--------------------|------------|
| 1.4 WAL — no | CRITICAL | Data loss, state divergence | Add minimal WAL before MVP |
| 1.3 at-least-once — HIGH | Deadlock, ack storms | Infinite retry loops, thrashing | Define retry/backoff/timeout |
| 1.2 type_id — MED | Type explosion, extensibility friction | 256+ types break schema | Plan migration to string topics |
| 7.3 LZ4 — MED | Bandwidth waste, CPU overhead | Network saturation | Add as runtime switch, profile first |
| 1.6 ordering — | Race conditions | Chunk processing order matters | Define "best effort" + sequence numbers |

**Immediate action required**: Define and implement WAL (1.4) before MVP deployment. This is the single highest risk in the ADR.
