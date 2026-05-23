# ADR Gap Analysis — Architectural Blind Spots

## Gap: Authentication & Security — MEDIUM
- **Not covered in ADR**: yes
- **Why it matters**: Router receives PlayerAction, ChunkRequest from Gateway. Without auth, any local process can inject fake actions, corrupt world state, or trigger multiblock formation maliciously.
- **Recommended decision**: Use mTLS between services (certificates baked into build). Client→Gateway uses JWT tokens. For MVP, HMAC with service-wide shared secret is sufficient.
- **Blocks**: Gateway message handling, Router state-changing deliveries

## Gap: Error Handling & Retries — MEDIUM
- **Not covered in ADR**: yes
- **Why it matters**: SimulationCore may query RecipeManager 20× per second. If RecipeManager crashes, SimulationCore will block forever. No ADR defines backoff, timeout, or degradation strategy.
- **Recommended decision**: Each Router RPC has `timeout_ms` and `max_retries` (exponential backoff). If RecipeManager is down, Router returns `RETRY_LATER` and SimulationCore caches last-known recipe (with TTL). For MVP, hard timeouts with 1 retry are acceptable.
- **Blocks**: SimulationCore tick loop, RecipeManager unavailability

## Gap: Configuration Management — HIGH
- **Not covered in ADR**: yes
- **Why it matters**: No ADR addresses how services configure their addresses, ports, or network timeouts. Hardcoded ports in code is brittle.
- **Recommended decision**: Each service reads from `config.toml` at startup. Key configs: `router_address`, `chunk_store_address`, `simulation_address`, `gateway_address`, `bind_port`. For MVP, command-line flags override file, environment variables override both.
- **Blocks**: All service startups, network connectivity

## Gap: Testing Strategy — HIGH
- **Not covered in ADR**: yes
- **Why it matters**: ADR describes service boundaries but no ADR defines how to test them. Unit tests don't catch protocol mismatches, routing bugs, or deadlock conditions.
- **Recommended decision**: Write one integration test per ADR scenario. Run in CI with Docker Compose spinning up all services.
- **Blocks**: CI pipeline, protocol correctness, regression testing

## Gap: Data Consistency — HIGH
- **Not covered in ADR**: partially (EntityStateStore "in-memory MVP + snapshot", but no race condition handling)
- **Why it matters**: No ADR defines ordering guarantees, snapshot frequency, or conflict resolution between ChunkStore and EntityStateStore.
- **Recommended decision**: EntityStateStore snapshots on: (1) state change, (2) chunk unload, (3) player disconnect, (4) every 5 seconds. Use `release` RPC to guarantee no concurrent reads.
- **Blocks**: SimulationCore state transitions, ChunkStore block changes

## Gap: Graceful Shutdown — MEDIUM
- **Not covered in ADR**: yes
- **Why it matters**: On deployment updates, Router must stop accepting new subscriptions but drain pending messages. No ADR defines shutdown order.
- **Recommended decision**: Each service installs SIGTERM handler. Shutdown order: ChunkStore → SimulationCore → RecipeManager → Gateway → Router.
- **Blocks**: Deployment, service crashes

## Gap: Player Session Management — HIGH
- **Not covered in ADR**: yes
- **Why it matters**: Client connects → Gateway accepts → sends ChunkSnapshot → creates Player entity. On disconnect, inventory must persist. On reconnect, chunk list must be recalculated.
- **Recommended decision**: Gateway maintains `PlayerSession{player_id, dim, last_chunk_list, inventory_snapshot}`. On disconnect: flush inventory to MetaDB, remove from `active_players`. On reconnect: reuse `player_id`, load saved inventory, recalculate chunk list.
- **Blocks**: Client connection flow, inventory persistence

## Gap: Dimension IDs — MEDIUM
- **Not covered in ADR**: ADR uses `dim` in `pack(dim, x, y, z)` but doesn't define wire representation.
- **Why it matters**: Router messages need dimension info. ChunkStore keys need dimension.
- **Recommended decision**: Use `uint32_t dim_id`. Default: Overworld=0, Nether=1, End=2. For MVP, hardcode overworld only and extend later.
- **Blocks**: ChunkStore persistence, Router message format

---

*Примечание: Service discovery и мониторинг не нужны — статическая конфигурация, Linux-only.*
