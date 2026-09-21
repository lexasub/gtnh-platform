## Context
MessageRouter currently has an in-process service registry and updates `lastSeen` on every inbound frame, but no network API exposes it. A heartbeat alone proves transport liveness, not that a service can process application work.

## Decisions
- Add Router control frames `HealthRequest` (0x06) and `HealthResponse` (0x07). Gateway sends one request with a nonce; Router returns a snapshot of registered services. Router probes each registered TCP connection with a health request/response exchange and records a monotonic response timestamp. Services answer the control frame in their existing router connection loops without changing business topics.
- A row contains service name, transport state, probe state, and `last_response_age_ms`. The gateway marks a service `DOWN` when its connection is closed or the probe times out, and `UNKNOWN` before its first response. A successful probe is `UP` and refreshes the response timestamp. No wall-clock timestamps cross processes.
- Gateway receives the Router snapshot asynchronously, wraps it in `ServiceHealthResp`, and sends it to the GameClient ctrl connection. The client requests snapshots with `ServiceHealthReq` and verifies the response before replacing its render-thread snapshot.
- Extend the existing `RenderBridge` Debug window rather than creating a modal `IUIWindow`. F3 toggles visibility through the existing ActionRegistry; Refresh issues a request. The overlay displays client connection state plus one line per service.

## Non-goals
- No topics, connection addresses, or service-specific business probes in the first version.
- No changes to existing message IDs or the stale `GatewayPayload` union; C++ `GatewayMsg` remains wire truth.

## Risks / Mitigations
- Older services must understand the new Router control frame. Router treats an unrecognized/absent response as `UNKNOWN`/`DOWN` after timeout; rollout remains compatible because the request is optional and services can ignore it. Current in-tree clients are updated together.
- Router control frames must not be delivered through pub/sub and must not block the router's main loop; use per-connection control state and bounded probe timeout.
