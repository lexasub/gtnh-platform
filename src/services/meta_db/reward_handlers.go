package main

import (
	"database/sql"
	"errors"
	"fmt"
	"log"
)

// RewardType represents the type of quest reward
const (
	RewardTypeItem = "item"
	RewardTypeExperience = "experience"
	RewardTypeSpecial = "special"
)

// rewardExecer is satisfied by both *sql.DB and *sql.Tx so quest rewards can
// be stored inside the exchange transaction.
type rewardExecer interface {
	Exec(query string, args ...any) (sql.Result, error)
}

// StorePlayerQuestReward stores a quest reward in the player_quest_rewards table
func StorePlayerQuestReward(
	db rewardExecer,
	playerID uint64,
	questID uint32,
	rewardType string,
	rewardID uint16,
	rewardCount uint8,
	rewardValue float64,
	redeemed uint8,
	timestamp int64,
	metadata string,
) error {

	_, err := db.Exec(
		"INSERT INTO player_quest_rewards (player_id, quest_id, reward_type, reward_id, reward_count, reward_value, redeemed, reward_timestamp, metadata) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
		playerID, questID, rewardType, rewardID, rewardCount, rewardValue, redeemed, timestamp, metadata,
	)
	if err != nil {
		return fmt.Errorf("failed to store quest reward: %w", err)
	}
	return nil
}

// GetPlayerQuestRewards retrieves all quest rewards for a player
func GetPlayerQuestRewards(db *sql.DB, playerID uint64) ([]PlayerQuestReward, error) {
	query := "SELECT id, player_id, quest_id, reward_type, reward_id, reward_count, reward_value, redeemed, reward_timestamp, metadata FROM player_quest_rewards WHERE player_id = ? ORDER BY reward_timestamp DESC"

	rows, err := db.Query(query, playerID)
	if err != nil {
		return nil, fmt.Errorf("failed to query quest rewards: %w", err)
	}
	defer rows.Close()

	var rewards []PlayerQuestReward
	for rows.Next() {
		var reward PlayerQuestReward
		err := rows.Scan(
			&reward.ID, &reward.PlayerID, &reward.QuestID, &reward.RewardType,
			&reward.RewardID, &reward.RewardCount, &reward.RewardValue,
			&reward.Redeemed, &reward.RewardTimestamp, &reward.Metadata,
		)
		if err != nil {
			return nil, fmt.Errorf("failed to scan quest reward: %w", err)
		}
		rewards = append(rewards, reward)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("error iterating quest rewards: %w", err)
	}

	return rewards, nil
}

// GetPlayerQuestRewardsByStatus retrieves quest rewards for a player filtered by redemption status
func GetPlayerQuestRewardsByStatus(db *sql.DB, playerID uint64, redeemed int) ([]PlayerQuestReward, error) {
	query := "SELECT id, player_id, quest_id, reward_type, reward_id, reward_count, reward_value, redeemed, reward_timestamp, metadata FROM player_quest_rewards WHERE player_id = ? AND redeemed = ? ORDER BY reward_timestamp DESC"

	rows, err := db.Query(query, playerID, redeemed)
	if err != nil {
		return nil, fmt.Errorf("failed to query quest rewards by status: %w", err)
	}
	defer rows.Close()

	var rewards []PlayerQuestReward
	for rows.Next() {
		var reward PlayerQuestReward
		err := rows.Scan(
			&reward.ID, &reward.PlayerID, &reward.QuestID, &reward.RewardType,
			&reward.RewardID, &reward.RewardCount, &reward.RewardValue,
			&reward.Redeemed, &reward.RewardTimestamp, &reward.Metadata,
		)
		if err != nil {
			return nil, fmt.Errorf("failed to scan quest reward: %w", err)
		}
		rewards = append(rewards, reward)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("error iterating quest rewards: %w", err)
	}

	return rewards, nil
}

// GetPlayerQuestRewardsByQuest retrieves quest rewards for a specific quest ID
func GetPlayerQuestRewardsByQuest(db *sql.DB, playerID uint64, questID uint32) ([]PlayerQuestReward, error) {
	query := "SELECT id, player_id, quest_id, reward_type, reward_id, reward_count, reward_value, redeemed, reward_timestamp, metadata FROM player_quest_rewards WHERE player_id = ? AND quest_id = ? ORDER BY reward_timestamp DESC"

	rows, err := db.Query(query, playerID, questID)
	if err != nil {
		return nil, fmt.Errorf("failed to query quest rewards by quest: %w", err)
	}
	defer rows.Close()

	var rewards []PlayerQuestReward
	for rows.Next() {
		var reward PlayerQuestReward
		err := rows.Scan(
			&reward.ID, &reward.PlayerID, &reward.QuestID, &reward.RewardType,
			&reward.RewardID, &reward.RewardCount, &reward.RewardValue,
			&reward.Redeemed, &reward.RewardTimestamp, &reward.Metadata,
		)
		if err != nil {
			return nil, fmt.Errorf("failed to scan quest reward: %w", err)
		}
		rewards = append(rewards, reward)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("error iterating quest rewards: %w", err)
	}

	return rewards, nil
}

const (
	// inventorySlotCount matches the positional inventory published by MetaDB
	// (see PublishInventoryTo: all 40 slots serialized positionally).
	inventorySlotCount = 40
	// maxStackSize is the per-slot stack limit for item rewards.
	maxStackSize = 64
)

// inventoryQueryer is satisfied by both *sql.DB and *sql.Tx so reward
// granting can run inside the redemption transaction, keeping grant +
// mark-redeemed atomic.
type inventoryQueryer interface {
	Query(query string, args ...any) (*sql.Rows, error)
	Exec(query string, args ...any) (sql.Result, error)
}

const upsertInventorySlotSQL = `
	INSERT INTO inventory (player_id, slot, block_id, count)
	VALUES (?, ?, ?, ?)
	ON CONFLICT(player_id, slot) DO UPDATE SET
		block_id = excluded.block_id,
		count = excluded.count
`

// grantItemToInventory adds `count` of `itemID` to the player's inventory,
// merging into existing partial stacks of the same block_id (up to 64 per
// slot) first, then filling the first empty slot (no row, or a row with
// block_id=0). Slots are limited to 0..39. If the inventory is full the call
// returns an error and nothing is granted for that item.
func grantItemToInventory(q inventoryQueryer, playerID uint64, itemID uint16, count uint8) error {
	if itemID == 0 || count == 0 {
		return nil
	}

	rows, err := q.Query("SELECT slot, block_id, count FROM inventory WHERE player_id = ? ORDER BY slot", playerID)
	if err != nil {
		return fmt.Errorf("failed to read inventory: %w", err)
	}

	type invSlot struct {
		blockID int
		count   int
	}
	slots := make(map[int]invSlot)
	for rows.Next() {
		var slot int
		var blockID, cnt int
		if err := rows.Scan(&slot, &blockID, &cnt); err != nil {
			rows.Close()
			return fmt.Errorf("failed to scan inventory row: %w", err)
		}
		slots[slot] = invSlot{blockID: blockID, count: cnt}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return fmt.Errorf("error iterating inventory rows: %w", err)
	}

	remaining := int(count)
	target := int(itemID)

	// 1. Merge into existing partial stacks of the same item.
	for slot, inv := range slots {
		if remaining == 0 {
			break
		}
		if inv.blockID == target && inv.count > 0 && inv.count < maxStackSize {
			add := maxStackSize - inv.count
			if add > remaining {
				add = remaining
			}
			inv.count += add
			remaining -= add
			slots[slot] = inv
			if _, err := q.Exec(upsertInventorySlotSQL, playerID, slot, inv.blockID, inv.count); err != nil {
				return fmt.Errorf("failed to update inventory slot %d: %w", slot, err)
			}
		}
	}

	// 2. Place any remainder into the first empty slot.
	if remaining > 0 {
		for slot := 0; slot < inventorySlotCount && remaining > 0; slot++ {
			inv, exists := slots[slot]
			if exists && inv.blockID != 0 {
				continue // occupied by a real item
			}
			add := remaining
			if add > maxStackSize {
				add = maxStackSize
			}
			if _, err := q.Exec(upsertInventorySlotSQL, playerID, slot, target, add); err != nil {
				return fmt.Errorf("failed to insert inventory slot %d: %w", slot, err)
			}
			remaining -= add
			slots[slot] = invSlot{blockID: target, count: add}
		}
	}

	if remaining > 0 {
		return fmt.Errorf("inventory full: cannot grant %d of item %d to player %d", count, itemID, playerID)
	}
	return nil
}

// RedeemPlayerQuestReward grants the reward to the player's inventory and
// marks it redeemed in a single transaction. Redeeming an already-redeemed
// row returns an error, so a reward can never be granted twice.
func (m *MetaDB) RedeemPlayerQuestReward(rewardID int64) error {
	tx, err := m.db.Begin()
	if err != nil {
		return fmt.Errorf("failed to begin transaction: %w", err)
	}
	defer tx.Rollback()

	var playerID uint64
	var questID uint32
	var rewardType string
	var rewardIDItem uint16
	var rewardCount uint8
	var currentRedeemed int

	err = tx.QueryRow(
		"SELECT player_id, quest_id, reward_type, reward_id, reward_count, redeemed FROM player_quest_rewards WHERE id = ?",
		rewardID,
	).Scan(&playerID, &questID, &rewardType, &rewardIDItem, &rewardCount, &currentRedeemed)
	if err != nil {
		if err == sql.ErrNoRows {
			return fmt.Errorf("quest reward not found: %d", rewardID)
		}
		return fmt.Errorf("failed to check quest reward status: %w", err)
	}

	if currentRedeemed == 1 {
		return fmt.Errorf("quest reward already redeemed: %d", rewardID)
	}

	// Grant item rewards into the player's inventory inside the same
	// transaction so a failed grant (e.g. inventory full) leaves the row
	// unredeemed and retryable.
	if rewardType == RewardTypeItem && rewardIDItem != 0 && rewardCount > 0 {
		if err := grantItemToInventory(tx, playerID, rewardIDItem, rewardCount); err != nil {
			return fmt.Errorf("failed to grant quest reward %d to player %d: %w", rewardID, playerID, err)
		}
	}

	result, err := tx.Exec("UPDATE player_quest_rewards SET redeemed = 1 WHERE id = ?", rewardID)
	if err != nil {
		return fmt.Errorf("failed to redeem quest reward: %w", err)
	}

	rowsAffected, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("failed to check rows affected: %w", err)
	}
	if rowsAffected == 0 {
		return fmt.Errorf("no rows updated - reward may have been redeemed concurrently: %d", rewardID)
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("failed to commit redemption transaction: %w", err)
	}

	// Log the redemption for audit purposes
	log.Printf("[REWARD] Quest reward redeemed: player=%d quest=%d reward_id=%d", playerID, questID, rewardID)

	return nil
}

// BatchRedeemPlayerQuestRewards grants and marks multiple quest rewards
// redeemed in a single transaction. All reward rows are validated (and items
// granted) before any row is marked redeemed; if any row fails, the whole
// transaction rolls back so no partial state can persist.
func (m *MetaDB) BatchRedeemPlayerQuestRewards(rewardIDs []int64) error {
	if len(rewardIDs) == 0 {
		return errors.New("no reward IDs provided")
	}

	tx, err := m.db.Begin()
	if err != nil {
		return fmt.Errorf("failed to begin transaction: %w", err)
	}
	defer tx.Rollback()

	// Validate all rewards can be redeemed and grant their items.
	for _, rewardID := range rewardIDs {
		var playerID uint64
		var questID uint32
		var rewardType string
		var rewardIDItem uint16
		var rewardCount uint8
		var currentRedeemed int

		err := tx.QueryRow(
			"SELECT player_id, quest_id, reward_type, reward_id, reward_count, redeemed FROM player_quest_rewards WHERE id = ?",
			rewardID,
		).Scan(&playerID, &questID, &rewardType, &rewardIDItem, &rewardCount, &currentRedeemed)
		if err != nil {
			if err == sql.ErrNoRows {
				return fmt.Errorf("quest reward not found: %d", rewardID)
			}
			return fmt.Errorf("failed to check quest reward status: %w", err)
		}

		if currentRedeemed == 1 {
			return fmt.Errorf("quest reward already redeemed: %d", rewardID)
		}

		if rewardType == RewardTypeItem && rewardIDItem != 0 && rewardCount > 0 {
			if err := grantItemToInventory(tx, playerID, rewardIDItem, rewardCount); err != nil {
				return fmt.Errorf("failed to grant quest reward %d to player %d: %w", rewardID, playerID, err)
			}
		}
	}

	// Perform all redemptions
	for _, rewardID := range rewardIDs {
		result, err := tx.Exec(
			"UPDATE player_quest_rewards SET redeemed = 1 WHERE id = ?",
			rewardID,
		)
		if err != nil {
			return fmt.Errorf("failed to redeem quest reward %d: %w", rewardID, err)
		}

		rowsAffected, err := result.RowsAffected()
		if err != nil {
			return fmt.Errorf("failed to check rows affected for reward %d: %w", rewardID, err)
		}

		if rowsAffected == 0 {
			return fmt.Errorf("no rows updated for reward %d - may have been redeemed concurrently", rewardID)
		}
	}

	// Commit the transaction
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("failed to commit redemption transaction: %w", err)
	}

	log.Printf("[REWARD] Batch redeemed %d quest rewards", len(rewardIDs))
	return nil
}

// grantStoredQuestReward finds the most recent pending (redeemed=0) reward
// row for a completed quest and redeems it, granting the item to the player's
// inventory. It is called right after the reward row is stored so quest
// completion delivers items immediately.
func (m *MetaDB) grantStoredQuestReward(playerID uint64, questID uint32) error {
	var rewardID int64
	err := m.db.QueryRow(
		"SELECT id FROM player_quest_rewards WHERE player_id = ? AND quest_id = ? AND redeemed = 0 ORDER BY id DESC LIMIT 1",
		playerID, questID,
	).Scan(&rewardID)
	if err != nil {
		if err == sql.ErrNoRows {
			return fmt.Errorf("no pending reward row for player=%d quest=%d", playerID, questID)
		}
		return fmt.Errorf("failed to find pending reward for player=%d quest=%d: %w", playerID, questID, err)
	}
	return m.RedeemPlayerQuestReward(rewardID)
}

// GetQuestDefinition retrieves quest definition data to determine rewards
func GetQuestDefinition(questID uint32) *QuestDef {
	def, ok := questDefs[questID]
	if !ok {
		return nil
	}
	return &def
}

// PlayerQuestReward represents a quest reward record
// This should match the player_quest_rewards table structure
// Note: This is a simplified version for demonstration
// In a real implementation, this would be imported from a common package

type PlayerQuestReward struct {
	ID            int64
	PlayerID      uint64
	QuestID       uint32
	RewardType    string
	RewardID      uint16
	RewardCount   uint8
	RewardValue   float64
	Redeemed      uint8
	RewardTimestamp int64
	Metadata      string
}

// QuestDef represents quest definition data
// This should match the quest definitions from quests.csv
// Note: This is a simplified version for demonstration
// In a real implementation, this would be imported from a common package

type QuestDef struct {
	ID           uint32
	Title        string
	RewardItemID uint16
	RewardCount  uint8
	Era          uint8
	Section      string
	DetectType   string
	CostItemID   uint16
	CostCount    uint8
	CooldownSecs uint16

	// Merged from quest_requirements.json. TargetItemID/TargetCount/DetectType
	// mirror the first requirement's objective; AutoComplete gates whether a
	// detected quest auto-completes or waits for a manual Complete button.
	AutoComplete bool
	TargetItemID uint16
	TargetCount  uint16
}

// GetQuestRewardsStats returns statistics about quest rewards for a player
func GetQuestRewardsStats(db *sql.DB, playerID uint64) (map[string]interface{}, error) {
	stats := make(map[string]interface{})

	// Total rewards earned
	var totalRewards int
	err := db.QueryRow("SELECT COUNT(*) FROM player_quest_rewards WHERE player_id = ?", playerID).Scan(&totalRewards)
	if err != nil {
		return nil, fmt.Errorf("failed to count total rewards: %w", err)
	}
	stats["total_rewards"] = totalRewards

	// Rewards redeemed
	var totalRedeemed int
	err = db.QueryRow("SELECT COUNT(*) FROM player_quest_rewards WHERE player_id = ? AND redeemed = 1", playerID).Scan(&totalRedeemed)
	if err != nil {
		return nil, fmt.Errorf("failed to count redeemed rewards: %w", err)
	}
	stats["total_redeemed"] = totalRedeemed

	// Rewards not redeemed
	stats["total_pending"] = totalRewards - totalRedeemed

	// Calculate redemption percentage
	if totalRewards > 0 {
		redeemedPercentage := float64(totalRedeemed) / float64(totalRewards) * 100
		stats["redemption_percentage"] = redeemedPercentage
	} else {
		stats["redemption_percentage"] = 0.0
	}

	// Most recent reward timestamp
	var recentTimestamp int64
	err = db.QueryRow("SELECT MAX(reward_timestamp) FROM player_quest_rewards WHERE player_id = ?", playerID).Scan(&recentTimestamp)
	if err != nil && err != sql.ErrNoRows {
		return nil, fmt.Errorf("failed to get recent timestamp: %w", err)
	}
	if err == nil {
		stats["most_recent_reward"] = recentTimestamp
	} else {
		stats["most_recent_reward"] = nil
	}

	return stats, nil
}