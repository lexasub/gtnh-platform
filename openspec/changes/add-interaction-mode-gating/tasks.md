# Tasks: Add Mode-Based Block Interaction Gating

## 1. Gate interaction by matrix
- [ ] 1.1 Replace the inline `ADVENTURE`/`SPECTATOR` check in `GameClient.cpp` with
      `GameModePerm::CanBreak` / `GameModePerm::CanPlace`
- [ ] 1.2 Confirm the break path and the place path both honor the predicate

## 2. Tests
- [ ] 2.1 Extend `tests/test_gamemode_permissions.cpp` (or a sibling) with
      `CanBreak`/`CanPlace` checks per mode
- [ ] 2.2 Integration: `/gamemode 2` and `/gamemode 3` block break/place;
      `/gamemode 0` and `/gamemode 1` allow it

## 3. Docs
- [ ] 3.1 Update `add-game-modes` (if still open) to reference this change for
      interaction semantics
