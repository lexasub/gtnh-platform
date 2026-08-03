## 1. Fix Counter Reset
- [x] 1.1 In `HeatTransferSystem` Pass 2, replace `emplace_or_replace<OverheatComponent>(ent, state, 0)` with: emplace only if absent, otherwise update `.state` in place — so `ticks_at_critical` is preserved across CRITICAL ticks
- [x] 1.2 Verify `ExplosionSystem` reaches `EXPLOSION_DELAY_TICKS = 60` consecutive CRITICAL ticks

## 2. Behavior
- [x] 2.1 Machine block set to air via `publishBlockChangedEvent` after 60 ticks (already in `ExplosionSystem.cpp:27-31`)
- [x] 2.2 ECS entity removed on explosion (already in `ExplosionSystem.cpp:36-38`)
- [x] 2.3 Boiler dry-run scenario: heat buildup with no water → CRITICAL → explosion

## 3. Verification
- [x] 3.1 Build: `cd cmake-build-debug && ninja -j5` — no new compilation errors
- [x] 3.2 Tests: `ctest --output-on-failure -j$(nproc)` — all pass
- [x] 3.3 LSP diagnostics clean on changed files
