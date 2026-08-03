# Change: Questbook Client UI Polish

## Why
The Quest Book window works (3-panel layout, status badges, network sync) but has deferred client-phase gaps: raw-binary parsing instead of FlatBuffers, hardcoded msgType `19`, no manual completion button, no unlock animation, no completion badges on era tabs, no era lock/unlock visuals.

## What Changes
- Refactor `updateQuestStatus()` + `OnNetworkUpdate()` to FlatBuffers + GatewayMsg enum constants.
- Manual completion button in detail view (server-authoritative validation via `QuestManager`).
- Unlock animation when new quests become available.
- Completion ratio badges on era tabs (e.g. "3/12").
- Era lock/unlock visual state (dimmed tabs).

## Impact
- Affected specs: questbook-client-polish (new)
- Affected code:
  - `src/services/game_client/UI/Windows/player/QuestBookWindow.cpp/.h`
  - `src/services/game_client/Network/NetClient.cpp/.h` — msgType 20/21 handling
