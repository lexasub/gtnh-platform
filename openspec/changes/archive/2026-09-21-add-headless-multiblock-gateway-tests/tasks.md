# Tasks: Headless Gateway multiblock testing

## 1. Documentation
- [x] 1.1 Document the automated client and diagnostic CLI in README.md and AGENTS.md.
- [x] 1.2 Add the focused headless-client guide with current limitations and the staged multiblock plan.

## 2. Shared client
- [x] 2.1 Replace partial TCP reads with `io.ReadFull` in test and CLI framing.
- [x] 2.2 Add request-ID-aware ACK waits and typed type-23 created/destroyed event decoders.
- [ ] 2.3 Add CLI multiblock watch/event output and explicit request/status controls.

## 3. Gateway E2E
- [x] 3.1 Add a unique-coordinate EBF fixture using PatternLibrary IDs; place controller last and assert every ACK.
- [x] 3.2 Verify `MultiblockCreatedEvent` anchor/type. Non-zero `mb_id` reads for every structure block are deferred until production metadata writes exist.
- [x] 3.3 Break the anchor block and verify anchor-only air teardown plus the destroyed lifecycle event. Non-anchor retention/cleanup assertions are follow-up work.

## 4. Persistence (follow-up)
*Moved to add-headless-multiblock-gateway-tests-verification*
## 5. Verification
*Moved to add-headless-multiblock-gateway-tests-verification*
