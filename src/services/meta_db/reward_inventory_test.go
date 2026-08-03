package main

import (
	"path/filepath"
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// newRewardTestMetaDB opens a MetaDB against a fresh temp-file SQLite database
// (in-memory would be per-connection with go-sqlite3 and is unsafe here).
func newRewardTestMetaDB(t *testing.T) *MetaDB {
	t.Helper()
	m, err := NewMetaDB(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatalf("NewMetaDB: %v", err)
	}
	t.Cleanup(func() { m.db.Close() })
	return m
}

// createTestPlayer inserts a bare player row so inventory/reward inserts
// satisfy the players foreign key.
func createTestPlayer(t *testing.T, m *MetaDB, playerID uint64) {
	t.Helper()
	if _, err := m.db.Exec("INSERT INTO players (id) VALUES (?)", playerID); err != nil {
		t.Fatalf("insert player %d: %v", playerID, err)
	}
}

// storeTestQuestReward stores an unredeemed item reward and returns its id.
func storeTestQuestReward(t *testing.T, m *MetaDB, playerID uint64, questID uint32, itemID uint16, count uint8) int64 {
	t.Helper()
	if err := StorePlayerQuestReward(m.db, playerID, questID, RewardTypeItem, itemID, count, 0, 0, time.Now().Unix(), "test-reward"); err != nil {
		t.Fatalf("StorePlayerQuestReward: %v", err)
	}
	var id int64
	if err := m.db.QueryRow(
		"SELECT id FROM player_quest_rewards WHERE player_id = ? AND quest_id = ? AND redeemed = 0 ORDER BY id DESC LIMIT 1",
		playerID, questID,
	).Scan(&id); err != nil {
		t.Fatalf("find reward row: %v", err)
	}
	return id
}

// countItemInInventory totals the stacks of the given item across all slots.
func countItemInInventory(m *MetaDB, playerID uint64, itemID uint16) int {
	slots, err := m.GetInventory(playerID)
	if err != nil {
		panic(err)
	}
	total := 0
	for _, s := range slots {
		if s.BlockID == itemID {
			total += int(s.Count)
		}
	}
	return total
}

// TestRedeemPlayerQuestRewardGrantsItemToInventory covers the core acceptance
// criterion: after a reward is stored, redeeming it places the item in the
// player's inventory and marks the row redeemed.
func TestRedeemPlayerQuestRewardGrantsItemToInventory(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(1001)
	createTestPlayer(t, m, playerID)

	rewardID := storeTestQuestReward(t, m, playerID, 7, 42, 5)

	if err := m.RedeemPlayerQuestReward(rewardID); err != nil {
		t.Fatalf("RedeemPlayerQuestReward: %v", err)
	}

	slots, err := m.GetInventory(playerID)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	found := false
	for _, s := range slots {
		if s.BlockID == 42 && s.Count == 5 {
			found = true
		}
	}
	if !found {
		t.Fatalf("expected item 42 count 5 in inventory, got %+v", slots)
	}

	var redeemed int
	if err := m.db.QueryRow("SELECT redeemed FROM player_quest_rewards WHERE id = ?", rewardID).Scan(&redeemed); err != nil {
		t.Fatalf("read redeemed: %v", err)
	}
	if redeemed != 1 {
		t.Fatalf("redeemed = %d, want 1", redeemed)
	}
}

// TestRedeemPlayerQuestRewardIsIdempotent covers the idempotency requirement:
// a second redeem attempt on the same row errors and must not duplicate items.
func TestRedeemPlayerQuestRewardIsIdempotent(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(1002)
	createTestPlayer(t, m, playerID)

	rewardID := storeTestQuestReward(t, m, playerID, 8, 42, 3)

	if err := m.RedeemPlayerQuestReward(rewardID); err != nil {
		t.Fatalf("first redeem: %v", err)
	}
	before := countItemInInventory(m, playerID, 42)

	if err := m.RedeemPlayerQuestReward(rewardID); err == nil {
		t.Fatal("expected second redeem to fail, got nil")
	}

	after := countItemInInventory(m, playerID, 42)
	if after != before {
		t.Fatalf("item count changed after re-redeem: before=%d after=%d", before, after)
	}
}

// TestRedeemPlayerQuestRewardMergesIntoExistingStack verifies reward items
// merge into an existing partial stack of the same item rather than a new slot.
func TestRedeemPlayerQuestRewardMergesIntoExistingStack(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(1003)
	createTestPlayer(t, m, playerID)

	if err := m.UpdateInventorySlot(playerID, 3, 42, 10); err != nil {
		t.Fatalf("seed inventory: %v", err)
	}

	rewardID := storeTestQuestReward(t, m, playerID, 9, 42, 5)
	if err := m.RedeemPlayerQuestReward(rewardID); err != nil {
		t.Fatalf("redeem: %v", err)
	}

	slots, err := m.GetInventory(playerID)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	if len(slots) != 1 || slots[0].Slot != 3 || slots[0].BlockID != 42 || slots[0].Count != 15 {
		t.Fatalf("expected single slot 3 with item 42 count 15, got %+v", slots)
	}
}

// TestRedeemPlayerQuestRewardFailsWhenInventoryFull verifies that a full
// inventory fails the redeem without granting anything and keeps the row
// redeemed=0 so the reward stays retryable.
func TestRedeemPlayerQuestRewardFailsWhenInventoryFull(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(1004)
	createTestPlayer(t, m, playerID)

	for s := 0; s < 40; s++ {
		if err := m.UpdateInventorySlot(playerID, s, 100+s, 1); err != nil {
			t.Fatalf("fill slot %d: %v", s, err)
		}
	}

	rewardID := storeTestQuestReward(t, m, playerID, 10, 42, 1)
	if err := m.RedeemPlayerQuestReward(rewardID); err == nil {
		t.Fatal("expected redeem to fail with a full inventory")
	}

	var redeemed int
	if err := m.db.QueryRow("SELECT redeemed FROM player_quest_rewards WHERE id = ?", rewardID).Scan(&redeemed); err != nil {
		t.Fatalf("read redeemed: %v", err)
	}
	if redeemed != 0 {
		t.Fatalf("redeemed = %d, want 0 (row kept retryable)", redeemed)
	}
	if got := countItemInInventory(m, playerID, 42); got != 0 {
		t.Fatalf("item 42 count = %d, want 0 (nothing granted)", got)
	}
}

// TestHandleQuestCompletedGrantsRewardToInventory exercises the full quest
// completion wiring: the event stores the reward row and immediately grants
// the item to the player's inventory (row left redeemed=1).
func TestHandleQuestCompletedGrantsRewardToInventory(t *testing.T) {
	m := newRewardTestMetaDB(t)
	playerID := uint64(1005)
	createTestPlayer(t, m, playerID)

	questID := uint32(9001)
	oldDefs := questDefs
	defer func() { questDefs = oldDefs }()
	questDefs = map[uint32]QuestDef{
		questID: {ID: questID, Title: "Test Quest", RewardItemID: 42, RewardCount: 5},
	}

	b := flatbuffers.NewBuilder(128)
	Protocol.QuestCompletedStart(b)
	Protocol.QuestCompletedAddPlayerId(b, playerID)
	Protocol.QuestCompletedAddQuestId(b, questID)
	Protocol.QuestCompletedAddTimestamp(b, uint64(time.Now().Unix()))
	off := Protocol.QuestCompletedEnd(b)
	b.Finish(off)

	HandleQuestCompleted("quest.completed", b.FinishedBytes(), m)

	if got := countItemInInventory(m, playerID, 42); got != 5 {
		t.Fatalf("inventory item 42 count = %d, want 5", got)
	}

	var pending int
	if err := m.db.QueryRow(
		"SELECT COUNT(*) FROM player_quest_rewards WHERE player_id = ? AND quest_id = ? AND redeemed = 0",
		playerID, questID,
	).Scan(&pending); err != nil {
		t.Fatalf("count pending: %v", err)
	}
	if pending != 0 {
		t.Fatalf("pending (redeemed=0) reward rows = %d, want 0 (auto-redeemed)", pending)
	}
}
