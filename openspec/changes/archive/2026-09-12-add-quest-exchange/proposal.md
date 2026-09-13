# Change: Add Quest Exchange System (repeatable item market)

## Why

Player starts without a crafting table and without inventory crafting. Quest 4 ("Craft a crafting table") was a deadlock. Instead of deleting it, we replace it with a **repeatable exchange quest** — a market: give N items → receive a reward, with a cooldown.

The quest **never completes** (stays AVAILABLE). The player can trade as many times as they want, once per cooldown window. This matches GTNH's market/quest-reward loop and gives a general "market" section in the quest book for trading raw materials for useful items.

## What Changes

- **New `DetectionType::EXCHANGE`** — quest is *not* completed by crafting/placing; it is a standing market offer
- **Repeatable semantics**: exchange quests stay `AVAILABLE` forever, never become `COMPLETED`, each exchange grants the reward through the existing reward path
- **`QuestDef` new fields**: `costItemId`, `costCount`, `cooldownSecs`
- **CSV new columns**: `cost_item`, `cost_count`, `cooldown` (appended after `reward_*` → 13 columns total)
- **MetaDB owns the whole exchange flow** (validate → deduct → cooldown → reward) — **no SimulationCore round-trip**. Client → Gateway → MetaDB via pub/sub topics
- **New protocol messages** (GatewayMsg wire types **26–29**, next free after `kQuestEraTransition=25`):
  - `QuestExchangeRequest` (26, client→server)
  - `QuestExchangeResponse` (27, server→client)
  - `QuestExchangeCooldownGet` (28, client→server)
  - `QuestExchangeCooldown` (29, server→client)
- **New cooldown table** `quest_exchange_cooldowns` in MetaDB (per-player, per-quest, server-authoritative timestamps)
- **SimulationCore integration is minimal**: exclude EXCHANGE quests from `BuildQuestEraMap()` (so `IsEraComplete()` works — exchange quests never complete) and reject EXCHANGE quests in `completeQuest()`
- **UI**: "Exchange" button in quest detail (EXCHANGE quests only), cost display ("Give: X xN → Receive: Y"), cooldown countdown, server error toast
- **Quest data**: resurrect quest 4 as exchange — 4 oak planks → 1 crafting table, cooldown 60s, in new `vagrant → market` section. Quests 5/38 stay root quests (they must NOT gate behind 4 — it never completes)

## Impact

- Affected specs: `questbook` (DetectionType, QuestDef, protocol, MetaDB storage, UI, era completion)
- Affected code: `QuestTypes.h`, `QuestData.h/cpp`, `QuestManager.h/cpp`, `quest.fbs`, `gateway.fbs`, `gateway.h/cpp`, `NetClient.h/cpp`, `QuestBookWindow.h/cpp`, MetaDB (`db.go`, `definitions.go`, `router_client.go`, new exchange handler)
- New DB table: `quest_exchange_cooldowns`
- New FlatBuffers tables: 4 (request/response/cooldown-get/cooldown)
- New pub/sub topics: `quest.exchange.request`, `quest.exchange.response`, `quest.exchange.cooldown.get`, `quest.exchange.cooldown.response`

## Non-Goals (for this change)

- NPC shops / currency system
- Multiple item costs per quest
- Cooldown persistence in SimulationCore (MetaDB only)
- Client-side cost validation (server-authoritative only)
- Quest completion for exchange quests (by design they never complete)
