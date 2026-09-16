# Change: Extend the headless Gateway client for multiblock E2E tests

## Why

The repository already has a GUI-free Gateway TCP client for integration tests and manual diagnostics, but multiblock coverage stops at in-process SimulationCore tests. The real Gateway path should exercise formation, `mb_id` metadata, destruction, and—after the EntityStateStore harness is available—persistence.

## What Changes

- Document the automated `GatewayClient` and `gateway_cli` in the project README, AGENTS.md, and a focused guide.
- Extend the shared headless client with reliable framed reads, request-correlated acknowledgements, and multiblock event decoding.
- Add a bounded end-to-end EBF scenario that builds the exact registered pattern through Gateway, observes `kMultiblockEvent`, and verifies anchor teardown. Per-block `mb_id` metadata and non-anchor cleanup remain follow-up work until production metadata writes support those contracts.
- Defer the separate persistence scenario with isolated EntityStateStore startup and direct type-4 state inspection to a follow-up milestone.

## Impact

- Affected specs: `headless-gateway-testing`.
- Affected code: `test/integration/testutil/`, `test/integration/`, `tools/gateway_cli/`, and integration-service startup.
- No existing Gateway message IDs change; `src/common/GatewayMsg.h` remains wire truth.
