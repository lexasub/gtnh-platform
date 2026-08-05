# Tasks: Add Quest Exchange System (repeatable market)

## 1. Protocol — FlatBuffers Schema

- [x] 1.1 Add `QuestExchangeRequest` table (quest_id: uint32) to `src/protocol/quest.fbs`
- [x] 1.2 Add `QuestExchangeResponse` table (quest_id: uint32, success: bool, error_message: string, cooldown_remaining_secs: uint32)
- [x] 1.3 Add `QuestExchangeCooldownGet` table (quest_id: uint32)
- [x] 1.4 Add `QuestExchangeCooldown` table (quest_id: uint32, cooldown_remaining_secs: uint32)
- [x] 1.5 Add union variants in `src/protocol/gateway.fbs`: `QuestExchangeRequest (26)`, `QuestExchangeResponse (27)`, `QuestExchangeCooldownGet (28)`, `QuestExchangeCooldown (29)` (note: C++ constants in gateway.h are authoritative for the wire)
- [x] 1.6 Add C++ constants in `src/services/gateway/gateway.h` and `src/services/game_client/Network/NetClient.h`: `kQuestExchangeRequest = 26`, `kQuestExchangeResponse = 27`, `kQuestExchangeCooldownGet = 28`, `kQuestExchangeCooldown = 29`
- [x] 1.7 Regenerate C++/Go FlatBuffers code (build step — flatc via CMake)

## 2. Quest Data (CSV + JSON)

- [x] 2.1 Extend `data/quests/quests.csv` header to 13 columns: append `cost_item, cost_count, cooldown` after `reward_item, reward_count`
- [x] 2.2 Add quest 4 back as exchange: `4 planks (0:10:00:0 x4) → 1 crafting table (0:10:11:1), cooldown 60s`, detect_type `exchange`, detect_target empty, section `market`, era `vagrant`
- [x] 2.3 Add quest 4 node to `data/quests/quest_graph.json` with prereqs=`[]` (root quest in market section)
- [x] 2.4 Keep quests 5 and 38 as root quests — **do NOT** gate them behind quest 4 (exchange quests never complete, would deadlock)

## 3. Core Data Model (C++ quest_lib)

- [x] 3.1 Add `EXCHANGE = 4` to `DetectionType` enum in `src/libs/quest_lib/QuestTypes.h`
- [x] 3.2 Add `DetectFromString("exchange")` mapping
- [x] 3.3 Add `costItemId`, `costCount`, `cooldownSecs` fields to `QuestDef` struct
- [x] 3.4 Extend `QuestData::LoadCSV()` to parse the 3 appended columns (indices 10, 11, 12); default 0 when empty/missing
- [x] 3.5 Exclude `EXCHANGE` quests from `QuestData::BuildQuestEraMap()` (quest_lib/QuestData.cpp:248) so `QuestGraph::IsEraComplete()` works
- [x] 3.6 Add unit test: era map excludes exchange quests; exchange quest never makes its era complete

## 4. QuestManager (C++ simulation_core)

- [x] 4.1 Guard `QuestManager::completeQuest()` (simulation_core/Quest/QuestManager.cpp:109): reject quests with `detectType == EXCHANGE` (log + return false)
- [x] 4.2 Add unit test: completeQuest rejects EXCHANGE quest

## 5. MetaDB (Go) — exchange ownership

- [x] 5.1 Create `quest_exchange_cooldowns` table (player_id, quest_id, expires_at; PK (player_id, quest_id)) in `src/services/meta_db/db.go`
- [x] 5.2 Extend `loadQuestDefinitions()` in `src/services/meta_db/definitions.go`: parse 13 columns (append cost_item/cost_count/cooldown), keep reward indices stable
- [x] 5.3 Subscribe to `quest.exchange.request` topic in `src/services/meta_db/router_client.go`
- [x] 5.4 Implement exchange handler: validate quest def (exists + EXCHANGE type), check cooldown, verify+deduct cost items in SQLite transaction, insert cooldown entry, grant reward via existing `StorePlayerQuestReward` path, publish `quest.exchange.response`
- [x] 5.5 Subscribe to `quest.exchange.cooldown.get` topic; publish `quest.exchange.cooldown.response` with remaining seconds (0 = none)
- [x] 5.6 Error codes: `unknown_quest`, `not_exchange`, `cooldown_active` (with remaining secs), `missing_items`
- [x] 5.7 Go tests: exchange success, cooldown rejection, missing items rejection, cooldown query

## 6. Gateway (C++)

- [x] 6.1 Dispatch case `kQuestExchangeRequest` (26) → publish `quest.exchange.request`
- [x] 6.2 Dispatch case `kQuestExchangeCooldownGet` (28) → publish `quest.exchange.cooldown.get`
- [x] 6.3 Subscribe to `quest.exchange.response` → forward to client as wire 27
- [x] 6.4 Subscribe to `quest.exchange.cooldown.response` → forward to client as wire 29

## 7. Client (C++ game_client)

- [x] 7.1 NetClient: `SendQuestExchangeRequest(questId)` (wire 26), `SendQuestExchangeCooldownGet(questId)` (wire 28)
- [x] 7.2 NetClient: dispatch cases for wire 27 (QuestExchangeResponse) and 29 (QuestExchangeCooldown) → route to onQuestUpdate_/UI callback
- [x] 7.3 QuestBookWindow QuestEntry/quest def: expose costItemId/costCount/cooldownSecs + dynamic cooldown remaining
- [x] 7.4 Quest detail view for EXCHANGE quests: show "Give: [cost_item] x[count] → Receive: [reward_item] x[count]" + cooldown duration
- [x] 7.5 "Exchange" button visible only for EXCHANGE quests; disabled + countdown when cooldown > 0; disabled + hint when player lacks cost items (best-effort, client-side)
- [x] 7.6 On quest detail open: send cooldown query; on response update button state
- [x] 7.7 On exchange response: success → toast + refresh cooldown; error → toast with error message

## 8. Verification

- [x] 8.1 Build: `ninja` in cmake-build-debug (regenerates flatbuffers)
- [x] 8.2 Run ctest: quest_lib + simulation_core tests (era map exclusion, completeQuest guard)
- [x] 8.3 Run Go tests: `cd src/services/meta_db && go test ./...`
- [x] 8.4 `openspec validate add-quest-exchange --strict`
