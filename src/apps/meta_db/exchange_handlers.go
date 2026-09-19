package main

import (
	"database/sql"
	"fmt"
	"log"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// HandleQuestExchangeRequest processes QuestExchangeRequest messages from the
// Gateway (relayed from the client on wire 26). MetaDB owns the exchange flow
// end-to-end: validate quest def, check cooldown, verify + deduct cost items,
// store cooldown, grant reward — all in a single SQLite transaction. The quest
// never completes (repeatable market). Replies on quest.exchange.response.
func HandleQuestExchangeRequest(topic string, payload []byte, m *MetaDB) {
	req := Protocol.GetRootAsQuestExchangeRequest(payload, 0)
	if req == nil {
		log.Printf("[EXCHANGE] HandleQuestExchangeRequest: failed to parse QuestExchangeRequest")
		return
	}

	playerID := req.PlayerId()
	questID := req.QuestId()
	log.Printf("[EXCHANGE] HandleQuestExchangeRequest: player=%d quest=%d", playerID, questID)

	reply := func(success bool, errMsg string, cooldownSecs uint32) {
		builder := flatbuffers.NewBuilder(128)
		var errOff flatbuffers.UOffsetT
		if errMsg != "" {
			errOff = builder.CreateString(errMsg)
		}
		Protocol.QuestExchangeResponseStart(builder)
		Protocol.QuestExchangeResponseAddPlayerId(builder, playerID)
		Protocol.QuestExchangeResponseAddQuestId(builder, questID)
		Protocol.QuestExchangeResponseAddSuccess(builder, success)
		Protocol.QuestExchangeResponseAddErrorMessage(builder, errOff)
		Protocol.QuestExchangeResponseAddCooldownRemainingSecs(builder, cooldownSecs)
		resp := Protocol.QuestExchangeResponseEnd(builder)
		builder.Finish(resp)
		if m.rc != nil {
			m.rc.PublishRaw("quest.exchange.response", builder.FinishedBytes())
		}
	}

	questDef := GetQuestDefinition(questID)
	if questDef == nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: unknown quest", playerID, questID)
		reply(false, "unknown_quest", 0)
		return
	}
	if questDef.DetectType != "exchange" {
		log.Printf("[EXCHANGE] player=%d quest=%d: not an exchange quest (detect=%s)", playerID, questID, questDef.DetectType)
		reply(false, "not_exchange", 0)
		return
	}

	now := time.Now().Unix()

	tx, err := m.db.Begin()
	if err != nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: begin tx: %v", playerID, questID, err)
		reply(false, "internal_error", 0)
		return
	}
	defer tx.Rollback()

	// Cooldown check: reject while an unexpired entry exists.
	var expiresAt int64
	err = tx.QueryRow(
		"SELECT expires_at FROM quest_exchange_cooldowns WHERE player_id = ? AND quest_id = ?",
		playerID, questID,
	).Scan(&expiresAt)
	if err == nil && expiresAt > now {
		remaining := uint32(expiresAt - now)
		log.Printf("[EXCHANGE] player=%d quest=%d: cooldown active, %ds remaining", playerID, questID, remaining)
		reply(false, "cooldown_active", remaining)
		return
	}
	if err != nil && err != sql.ErrNoRows {
		log.Printf("[EXCHANGE] player=%d quest=%d: cooldown check failed: %v", playerID, questID, err)
		reply(false, "internal_error", 0)
		return
	}

	// Verify the player has enough of the cost item.
	costItemID := int(questDef.CostItemID)
	costCount := int(questDef.CostCount)
	if costItemID == 0 || costCount == 0 {
		log.Printf("[EXCHANGE] player=%d quest=%d: quest has no cost configured", playerID, questID)
		reply(false, "missing_items", 0)
		return
	}

	type invRow struct {
		slot  int
		count int
	}
	rows, err := tx.Query(
		"SELECT slot, count FROM inventory WHERE player_id = ? AND block_id = ? AND count > 0 ORDER BY slot",
		playerID, costItemID,
	)
	if err != nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: inventory query failed: %v", playerID, questID, err)
		reply(false, "internal_error", 0)
		return
	}
	var costRows []invRow
	have := 0
	for rows.Next() {
		var r invRow
		if err := rows.Scan(&r.slot, &r.count); err != nil {
			rows.Close()
			log.Printf("[EXCHANGE] player=%d quest=%d: scan inventory: %v", playerID, questID, err)
			reply(false, "internal_error", 0)
			return
		}
		costRows = append(costRows, r)
		have += r.count
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: iterate inventory: %v", playerID, questID, err)
		reply(false, "internal_error", 0)
		return
	}

	if have < costCount {
		log.Printf("[EXCHANGE] player=%d quest=%d: missing items, have=%d need=%d", playerID, questID, have, costCount)
		reply(false, "missing_items", 0)
		return
	}

	// Deduct cost items, consuming full stacks first.
	remaining := costCount
	for _, r := range costRows {
		if remaining <= 0 {
			break
		}
		take := r.count
		if take > remaining {
			take = remaining
		}
		newCount := r.count - take
		if newCount == 0 {
			if _, err := tx.Exec("DELETE FROM inventory WHERE player_id = ? AND slot = ?", playerID, r.slot); err != nil {
				log.Printf("[EXCHANGE] player=%d quest=%d: delete slot %d: %v", playerID, questID, r.slot, err)
				reply(false, "internal_error", 0)
				return
			}
		} else {
			if _, err := tx.Exec("UPDATE inventory SET count = ? WHERE player_id = ? AND slot = ?", newCount, playerID, r.slot); err != nil {
				log.Printf("[EXCHANGE] player=%d quest=%d: update slot %d: %v", playerID, questID, r.slot, err)
				reply(false, "internal_error", 0)
				return
			}
		}
		remaining -= take
	}

	// Store cooldown (upsert: a retry after expiry refreshes expires_at).
	newExpires := now + int64(questDef.CooldownSecs)
	if _, err := tx.Exec(
		`INSERT INTO quest_exchange_cooldowns (player_id, quest_id, expires_at) VALUES (?, ?, ?)
		 ON CONFLICT(player_id, quest_id) DO UPDATE SET expires_at = excluded.expires_at`,
		playerID, questID, newExpires,
	); err != nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: store cooldown: %v", playerID, questID, err)
		reply(false, "internal_error", 0)
		return
	}

	// Grant the reward: store a reward row (redeemed=1 — the item is granted
	// in the same transaction; a grant failure rolls back the whole exchange,
	// so no pending row can be double-redeemed later).
	rewardType := "item"
	metadata := fmt.Sprintf("quest_id=%d,era=%d,section=%s,exchange", questID, questDef.Era, questDef.Section)
	if err := StorePlayerQuestReward(tx, playerID, questID, rewardType, questDef.RewardItemID, questDef.RewardCount, 0, 1, now, metadata); err != nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: store reward: %v", playerID, questID, err)
		reply(false, "internal_error", 0)
		return
	}
	if err := grantItemToInventory(tx, playerID, questDef.RewardItemID, questDef.RewardCount); err != nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: grant reward: %v", playerID, questID, err)
		reply(false, "inventory_full", 0)
		return
	}

	if err := tx.Commit(); err != nil {
		log.Printf("[EXCHANGE] player=%d quest=%d: commit: %v", playerID, questID, err)
		reply(false, "internal_error", 0)
		return
	}

	log.Printf("[EXCHANGE] player=%d quest=%d: exchange complete, reward item=%d x%d, cooldown=%ds",
		playerID, questID, questDef.RewardItemID, questDef.RewardCount, questDef.CooldownSecs)
	reply(true, "", uint32(questDef.CooldownSecs))
}

// HandleQuestExchangeCooldownGet processes QuestExchangeCooldownGet messages
// from the Gateway (wire 28). Returns the remaining cooldown in seconds
// (0 = no cooldown) via quest.exchange.cooldown.response.
func HandleQuestExchangeCooldownGet(topic string, payload []byte, m *MetaDB) {
	req := Protocol.GetRootAsQuestExchangeCooldownGet(payload, 0)
	if req == nil {
		log.Printf("[EXCHANGE] HandleQuestExchangeCooldownGet: failed to parse QuestExchangeCooldownGet")
		return
	}

	playerID := req.PlayerId()
	questID := req.QuestId()
	now := time.Now().Unix()

	var remaining uint32
	var expiresAt int64
	err := m.db.QueryRow(
		"SELECT expires_at FROM quest_exchange_cooldowns WHERE player_id = ? AND quest_id = ?",
		playerID, questID,
	).Scan(&expiresAt)
	if err == nil && expiresAt > now {
		remaining = uint32(expiresAt - now)
	}
	if err != nil && err != sql.ErrNoRows {
		log.Printf("[EXCHANGE] cooldown query failed for player=%d quest=%d: %v", playerID, questID, err)
	}

	builder := flatbuffers.NewBuilder(64)
	Protocol.QuestExchangeCooldownStart(builder)
	Protocol.QuestExchangeCooldownAddPlayerId(builder, playerID)
	Protocol.QuestExchangeCooldownAddQuestId(builder, questID)
	Protocol.QuestExchangeCooldownAddCooldownRemainingSecs(builder, remaining)
	resp := Protocol.QuestExchangeCooldownEnd(builder)
	builder.Finish(resp)

	if m.rc != nil {
		m.rc.PublishRaw("quest.exchange.cooldown.response", builder.FinishedBytes())
	}
	log.Printf("[EXCHANGE] cooldown response: player=%d quest=%d remaining=%ds", playerID, questID, remaining)
}
