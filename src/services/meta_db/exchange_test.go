package main

import (
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestPackItemSpec(t *testing.T) {
	cases := []struct {
		spec string
		want uint16
	}{
		{"0:10:3", 0x4003},
		{"10:0", 0x8000},
		{"1111:0:5", 0xF005},
		{"0:10:11:1", 0x5801},
		{"0:10:00:0", 0x4000},
		{"1110:00:0", 0xE000},
		{"", 0},
		{"0", 0},
		{"5", 5},
	}
	for _, c := range cases {
		if got := packItemSpec(c.spec); got != c.want {
			t.Errorf("packItemSpec(%q) = %#x, want %#x", c.spec, got, c.want)
		}
	}
}

func buildExchangeRequest(playerID uint64, questID uint32) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.QuestExchangeRequestStart(b)
	Protocol.QuestExchangeRequestAddPlayerId(b, playerID)
	Protocol.QuestExchangeRequestAddQuestId(b, questID)
	off := Protocol.QuestExchangeRequestEnd(b)
	b.Finish(off)
	return b.FinishedBytes()
}

func parseExchangeResponse(t *testing.T, payload []byte) (bool, string, uint32) {
	t.Helper()
	resp := Protocol.GetRootAsQuestExchangeResponse(payload, 0)
	if resp == nil {
		t.Fatal("nil QuestExchangeResponse")
	}
	return resp.Success(), string(resp.ErrorMessage()), resp.CooldownRemainingSecs()
}

// seedExchangeQuestDef installs an exchange quest definition and returns a
// restore func (mirrors the pattern in reward_inventory_test.go).
func seedExchangeQuestDef(t *testing.T, questID uint32, costID uint16, costCount uint8, rewardID uint16, rewardCount uint8, cooldown uint16) func() {
	t.Helper()
	oldDefs := questDefs
	questDefs = map[uint32]QuestDef{
		questID: {
			ID: questID, Title: "Exchange Test", RewardItemID: rewardID,
			RewardCount: rewardCount, DetectType: "exchange",
			CostItemID: costID, CostCount: costCount, CooldownSecs: cooldown,
		},
	}
	return func() { questDefs = oldDefs }
}

func TestHandleQuestExchangeRequestSuccess(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(2001)
	createTestPlayer(t, m, playerID)

	const questID = uint32(4)
	const costItem = uint16(0x4000)  // oak planks
	const rewardItem = uint16(0x5801) // crafting table
	restore := seedExchangeQuestDef(t, questID, costItem, 4, rewardItem, 1, 60)
	defer restore()

	if err := m.UpdateInventorySlot(playerID, 0, int(costItem), 4); err != nil {
		t.Fatalf("seed inventory: %v", err)
	}

	HandleQuestExchangeRequest("quest.exchange.request", buildExchangeRequest(playerID, questID), m)

	// Cost deducted, reward granted, cooldown stored.
	if got := countItemInInventory(m, playerID, costItem); got != 0 {
		t.Errorf("cost item remaining = %d, want 0", got)
	}
	if got := countItemInInventory(m, playerID, rewardItem); got != 1 {
		t.Errorf("reward item count = %d, want 1", got)
	}

	var expiresAt int64
	if err := m.db.QueryRow(
		"SELECT expires_at FROM quest_exchange_cooldowns WHERE player_id = ? AND quest_id = ?",
		playerID, questID,
	).Scan(&expiresAt); err != nil {
		t.Fatalf("cooldown row: %v", err)
	}
	if expiresAt <= time.Now().Unix() {
		t.Errorf("expires_at = %d, want in the future", expiresAt)
	}

	var pending int
	if err := m.db.QueryRow(
		"SELECT COUNT(*) FROM player_quest_rewards WHERE player_id = ? AND quest_id = ? AND redeemed = 0",
		playerID, questID,
	).Scan(&pending); err != nil {
		t.Fatalf("count pending: %v", err)
	}
	if pending != 0 {
		t.Errorf("pending (redeemed=0) reward rows = %d, want 0 (exchange grants in-tx)", pending)
	}
}

func TestHandleQuestExchangeRequestMissingItems(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(2002)
	createTestPlayer(t, m, playerID)

	const questID = uint32(4)
	restore := seedExchangeQuestDef(t, questID, 0x4000, 4, 0x5801, 1, 60)
	defer restore()

	// Only 2 of 4 required planks.
	if err := m.UpdateInventorySlot(playerID, 0, 0x4000, 2); err != nil {
		t.Fatalf("seed inventory: %v", err)
	}

	HandleQuestExchangeRequest("quest.exchange.request", buildExchangeRequest(playerID, questID), m)

	if got := countItemInInventory(m, playerID, 0x4000); got != 2 {
		t.Errorf("cost item after failed exchange = %d, want 2 (unchanged)", got)
	}
	if got := countItemInInventory(m, playerID, 0x5801); got != 0 {
		t.Errorf("reward item after failed exchange = %d, want 0", got)
	}
	var cooldownRows int
	if err := m.db.QueryRow(
		"SELECT COUNT(*) FROM quest_exchange_cooldowns WHERE player_id = ? AND quest_id = ?",
		playerID, questID,
	).Scan(&cooldownRows); err != nil {
		t.Fatalf("count cooldown: %v", err)
	}
	if cooldownRows != 0 {
		t.Errorf("cooldown rows = %d, want 0 (no cooldown on failure)", cooldownRows)
	}
}

func TestHandleQuestExchangeRequestCooldownActive(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(2003)
	createTestPlayer(t, m, playerID)

	const questID = uint32(4)
	const costItem = uint16(0x4000)
	restore := seedExchangeQuestDef(t, questID, costItem, 4, 0x5801, 1, 60)
	defer restore()

	// Successful exchange first, then refill cost and try again within cooldown.
	if err := m.UpdateInventorySlot(playerID, 0, int(costItem), 4); err != nil {
		t.Fatalf("seed inventory: %v", err)
	}
	HandleQuestExchangeRequest("quest.exchange.request", buildExchangeRequest(playerID, questID), m)

	// Refill at slot 1: slot 0 now holds the granted crafting table.
	if err := m.UpdateInventorySlot(playerID, 1, int(costItem), 4); err != nil {
		t.Fatalf("refill inventory: %v", err)
	}
	HandleQuestExchangeRequest("quest.exchange.request", buildExchangeRequest(playerID, questID), m)

	if got := countItemInInventory(m, playerID, 0x5801); got != 1 {
		t.Errorf("reward item count = %d, want 1 (second exchange rejected)", got)
	}
	if got := countItemInInventory(m, playerID, costItem); got != 4 {
		t.Errorf("cost item after rejected exchange = %d, want 4 (not deducted)", got)
	}
}

func TestHandleQuestExchangeRequestUnknownAndNotExchange(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(2004)
	createTestPlayer(t, m, playerID)

	// Unknown quest id (no def installed).
	HandleQuestExchangeRequest("quest.exchange.request", buildExchangeRequest(playerID, 9999), m)

	// A non-exchange quest def.
	restore := seedExchangeQuestDef(t, 1, 0x4000, 4, 0x5801, 1, 60)
	defer restore()
	oldDefs := questDefs
	questDefs = map[uint32]QuestDef{
		1: {ID: 1, Title: "Craft Quest", DetectType: "craft"},
	}
	defer func() { questDefs = oldDefs }()
	HandleQuestExchangeRequest("quest.exchange.request", buildExchangeRequest(playerID, 1), m)
}

func TestHandleQuestExchangeCooldownGet(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(2005)
	createTestPlayer(t, m, playerID)

	const questID = uint32(4)
	restore := seedExchangeQuestDef(t, questID, 0x4000, 4, 0x5801, 1, 60)
	defer restore()

	if err := m.UpdateInventorySlot(playerID, 0, 0x4000, 4); err != nil {
		t.Fatalf("seed inventory: %v", err)
	}
	HandleQuestExchangeRequest("quest.exchange.request", buildExchangeRequest(playerID, questID), m)

	b := flatbuffers.NewBuilder(64)
	Protocol.QuestExchangeCooldownGetStart(b)
	Protocol.QuestExchangeCooldownGetAddPlayerId(b, playerID)
	Protocol.QuestExchangeCooldownGetAddQuestId(b, questID)
	off := Protocol.QuestExchangeCooldownGetEnd(b)
	b.Finish(off)

	HandleQuestExchangeCooldownGet("quest.exchange.cooldown.get", b.FinishedBytes(), m)

	// No cooldown for an unknown quest → 0 remaining (handler must not panic).
	Protocol.QuestExchangeCooldownGetStart(b)
	Protocol.QuestExchangeCooldownGetAddPlayerId(b, playerID)
	Protocol.QuestExchangeCooldownGetAddQuestId(b, 7777)
	off = Protocol.QuestExchangeCooldownGetEnd(b)
	b.Finish(off)
	HandleQuestExchangeCooldownGet("quest.exchange.cooldown.get", b.FinishedBytes(), m)
}
