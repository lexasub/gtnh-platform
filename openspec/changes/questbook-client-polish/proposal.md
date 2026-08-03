# Change: Quest Book Client UI Polish

## Why
The Quest Book window renders (3-panel layout, status badges, network sync) but lacks client polish: no visual notifications on quest unlock/completion, no era progression indicators (completion ratio badges, locked-era dimming), and the quest wire contract is split between `gateway.fbs` (19/20/21) and the live wire constants (20/21/22). Manual quest completion needs a full cross-service protocol plus reward plumbing and is tracked separately in `manual-completion`.

## What Changes
- **Wire contract**: correct the quest numbering in `gateway.fbs` to match the live wire (20/21/22); all references use `GatewayMsg` enum constants. (The FlatBuffers parsing refactor itself already landed in `158038a`.)
- **Unlock notification** visuals on `QuestUnlockNotification` (20) — banner/toast + highlight newly unlocked quests in the list. (The server-side producer on `quest.unlocked` is tracked in `questbook-detection-handlers`.)
- **Completion notification** visuals + reward info on `QuestCompletedNotification` (21).
- **Era completion badges** on era tabs (e.g. "3/12").
- **Era lock/unlock visual state** — locked eras dimmed/disabled until the preceding era is complete. (The era-transition mechanics are tracked in `questbook-era-transition`.)
- **Client→server routing**: `NetClient` quest get/set senders + gateway forward to `meta_db.quest.get` / `meta_db.quest.set`.

## Impact
- Affected specs: questbook-client-polish (new)
- Affected code:
  - `src/services/game_client/UI/Windows/player/QuestBookWindow.cpp/.h`
  - `src/services/game_client/Network/NetClient.cpp/.h`
  - `src/services/gateway/gateway.cpp/.h`, `main.cpp`
  - `src/protocol/gateway.fbs`
- Consumes (event producers, separate changes): `questbook-detection-handlers` (`quest.unlocked`), `questbook-era-transition` (era transition)
- Out of scope: the manual completion button → `manual-completion` (server-authoritative). The `quest.set` transport added here SHALL NOT become the manual-completion path — status-mutation validation lives server-side in `manual-completion`.
