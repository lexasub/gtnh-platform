# Design: Quest Exchange System (repeatable market)

## Context

Current quest detection types (CRAFT, BLOCK_PLACED, TOOL_CHARGED, SIDE_CONFIGURED) are passive — the system watches player actions and auto-completes. EXCHANGE is different: a standing market offer. The player clicks "Exchange", the server consumes cost items, grants the reward, and starts a cooldown. The quest stays AVAILABLE — it is never COMPLETED.

## Goals / Non-Goals

- Goals: repeatable exchange quests, server-authoritative cost deduction, cooldown enforcement, reward delivery through the existing reward path, zero disruption to era completion.
- Non-Goals: quest completion for exchange quests, NPC integration, multi-item costs, currency system, cooldowns in SimulationCore.

## Decisions

### Decision 1: Exchange quests never complete — repeatable market

**Why**: The whole point of the market is repeatability (like GTNH market). If the quest became COMPLETED after one trade, the market would be dead.

Consequences (all handled):
- Quest stays `AVAILABLE` forever; `QuestProgressUpdate` continues to show AVAILABLE
- Exchange quests are **excluded from `BuildQuestEraMap()`** — otherwise the `vagrant` era would never complete (its map includes quest 4, which never completes) and the Apprentice era would never unlock
- `completeQuest()` rejects EXCHANGE-type quests (guard against accidental auto-completion)
- Nothing may use an exchange quest as a prerequisite — quest 5 and quest 38 remain root quests

### Decision 2: MetaDB owns the entire exchange flow — no SimulationCore round-trip

**Why**: MetaDB already owns player inventory and quest rewards. Routing the exchange through SimulationCore would add a hop for no benefit (SimulationCore has no inventory data). MetaDB validates the quest definition (it loads `quests.csv` itself), checks the cooldown, deducts items, stores the cooldown, and grants the reward — all in one process, one SQLite transaction.

Flow:
```
[Quest Detail UI: "Exchange" button]
→ Client: SendQuestExchangeRequest(questId)         [wire 26]
→ Gateway: case 26 → publish "quest.exchange.request"
→ MetaDB handler (subscribe "quest.exchange.request"):
   1. quest def exists && detectType == EXCHANGE      → else error "not_exchange"/"unknown_quest"
   2. cooldown expired (quest_exchange_cooldowns)     → else error "cooldown_active" (with remaining secs)
   3. player has costItem × costCount                  → else error "missing_items"
   4. SQLite txn: deduct cost items + insert cooldown entry (expires_at = now + cooldownSecs)
   5. grant reward via existing player_quest_rewards path
   → publish "quest.exchange.response"
→ Gateway (subscribe "quest.exchange.response") → forward to client [wire 27]
```

### Decision 3: Cooldown query is explicit request/response, not push

**Why**: The client needs the remaining cooldown when opening a quest detail view, but there's no real-time push channel for per-quest timers. A dedicated query keeps it pull-based and simple.

```
Client: SendQuestExchangeCooldownGet(questId)        [wire 28]
→ Gateway: case 28 → publish "quest.exchange.cooldown.get"
→ MetaDB handler → publish "quest.exchange.cooldown.response" (remaining_secs, 0 = none)
→ Gateway (subscribe) → forward to client            [wire 29]
```

### Decision 4: Cooldown stored in MetaDB, server-authoritative

New table `quest_exchange_cooldowns(player_id, quest_id, expires_at)`, PK `(player_id, quest_id)`.

**Why**: Cooldowns must survive server restarts, and timestamps must be server-side (client clock cannot be trusted). A separate table keeps `quest_progress` clean (exchange quests never write progress rows).

### Decision 5: Cost validation server-authoritative only; client shows best-effort hints

Client displays cost from its local quest definition and can hint "not enough items", but the server always re-validates inside the SQLite transaction before deducting. Partial failure (deduct succeeds, reward fails) is impossible — same transaction.

### Decision 6: FlatBuffers protocol additions (wire types 26–29)

`gateway.h`/`NetClient.h` constants are the source of truth for the C++ enum (note: gateway.fbs union numbers are known-stale for quest messages — update the union, but the C++ constants 26–29 govern the wire).

```
table QuestExchangeRequest { quest_id: uint32; }

table QuestExchangeResponse {
  quest_id: uint32;
  success: bool;
  error_message: string;            // "" on success; "missing_items" | "cooldown_active" | "not_exchange" | "unknown_quest"
  cooldown_remaining_secs: uint32;  // on success: full cooldown; on cooldown_active: remaining
}

table QuestExchangeCooldownGet { quest_id: uint32; }

table QuestExchangeCooldown {
  quest_id: uint32;
  cooldown_remaining_secs: uint32;  // 0 = no cooldown
}
```

Wire mapping:
- `kQuestExchangeRequest = 26` → `QuestExchangeRequest` (client→gateway, case in dispatch)
- `kQuestExchangeResponse = 27` → `QuestExchangeResponse` (gateway→client, topic forward)
- `kQuestExchangeCooldownGet = 28` → `QuestExchangeCooldownGet` (client→gateway, case in dispatch)
- `kQuestExchangeCooldown = 29` → `QuestExchangeCooldown` (gateway→client, topic forward)

Topics:
- `quest.exchange.request` — gateway publish / MetaDB subscribe
- `quest.exchange.response` — MetaDB publish / gateway subscribe → wire 27
- `quest.exchange.cooldown.get` — gateway publish / MetaDB subscribe
- `quest.exchange.cooldown.response` — MetaDB publish / gateway subscribe → wire 29

### Decision 7: CSV format additions (append, backward-compatible indices)

`quests.csv` grows from 10 to 13 columns. New columns are **appended after** `reward_item,reward_count` so existing reward-column indices (8, 9) stay stable:

```
... detect_type, detect_target, reward_item, reward_count, cost_item, cost_count, cooldown
```

- `cost_item` — item ID to consume (hierarchical packed, same format as detect_target)
- `cost_count` — quantity to consume (uint8)
- `cooldown` — cooldown in seconds (uint16), 0 = no cooldown

Empty/missing → default 0 (non-exchange quests unaffected).

## Risks / Trade-offs

- **Era deadlock if exclusion missed**: if an exchange quest leaks into `BuildQuestEraMap()`, its era never completes. → `BuildQuestEraMap()` skips `EXCHANGE`; add a unit test.
- **Accidental completion via `completeQuest`**: a future caller could complete an exchange quest. → explicit guard in `completeQuest()` + test.
- **Reward spam**: repeatable reward grants could flood `player_quest_rewards`. → cooldown enforced server-side; each grant is a separate row (existing schema supports it). Cooldown 0 = no cooldown is allowed for test quests but production quests set it.
- **Client clock drift**: client shows a countdown for UX only. → enforcement is server-authoritative timestamps only.

## Migration Plan

1. Add `quest_exchange_cooldowns` table to MetaDB schema (idempotent CREATE TABLE IF NOT EXISTS)
2. Add FlatBuffers tables to `quest.fbs` + union variants in `gateway.fbs`; add C++ constants 26–29 in `gateway.h`/`NetClient.h`
3. Regenerate FlatBuffers C++/Go code (build step)
4. Extend `QuestDef` with costItemId/costCount/cooldownSecs; extend CSV parser (13 cols, appended)
5. Exclude EXCHANGE from `BuildQuestEraMap()`; guard `completeQuest()`
6. Implement MetaDB exchange handler + cooldown query + router subscriptions
7. Implement Gateway wire cases (26/28) and topic forwards (27/29)
8. Implement client `QuestBookWindow` Exchange button + countdown
9. Re-add quest 4 to `quests.csv`/`quest_graph.json` as exchange (4 planks → crafting table, 60s, vagrant/market). Quests 5/38 stay root.

## Open Questions

- Should the client also show a cooldown on already-traded quests when re-opening? Yes — cooldown query runs on every quest detail open (cheap, pull-based).
- What if the player's inventory is full when the reward is granted? MetaDB grants the reward row regardless (rewards are redeemable rows, not direct inventory inserts in the current path) — same behavior as quest.completed rewards.
