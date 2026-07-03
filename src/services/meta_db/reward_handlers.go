package main

import (
	"database/sql"
	"fmt"
	"log"
	"errors"
)

// RewardType represents the type of quest reward
const (
	RewardTypeItem = "item"
	RewardTypeExperience = "experience"
	RewardTypeSpecial = "special"
)

// StorePlayerQuestReward stores a quest reward in the player_quest_rewards table
func StorePlayerQuestReward(
	db *sql.DB,
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

// RedeemPlayerQuestReward marks a quest reward as redeemed with validation
func RedeemPlayerQuestReward(db *sql.DB, rewardID int64) error {
	// First, verify the reward exists and is not already redeemed
	var currentRedeemed int
	var playerID uint64
	var questID uint32
	
	err := db.QueryRow(
		"SELECT player_id, quest_id, redeemed FROM player_quest_rewards WHERE id = ?",
		rewardID,
	).Scan(&playerID, &questID, &currentRedeemed)
	if err != nil {
		if err == sql.ErrNoRows {
			return fmt.Errorf("quest reward not found: %d", rewardID)
		}
		return fmt.Errorf("failed to check quest reward status: %w", err)
	}

	if currentRedeemed == 1 {
		return fmt.Errorf("quest reward already redeemed: %d", rewardID)
	}

	// Perform the redemption update
	result, err := db.Exec(
		"UPDATE player_quest_rewards SET redeemed = 1 WHERE id = ?",
		rewardID,
	)
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

	// Log the redemption for audit purposes
	log.Printf("[REWARD] Quest reward redeemed: player=%d quest=%d reward_id=%d", playerID, questID, rewardID)

	return nil
}

// BatchRedeemPlayerQuestRewards redemptions multiple quest rewards in a transaction
func BatchRedeemPlayerQuestRewards(db *sql.DB, rewardIDs []int64) error {
	if len(rewardIDs) == 0 {
		return errors.New("no reward IDs provided")
	}

	tx, err := db.Begin()
	if err != nil {
		return fmt.Errorf("failed to begin transaction: %w", err)
	}
	defer tx.Rollback()

	// Validate all rewards can be redeemed
	for _, rewardID := range rewardIDs {
		var currentRedeemed int
		var playerID uint64
		var questID uint32

		err := tx.QueryRow(
			"SELECT player_id, quest_id, redeemed FROM player_quest_rewards WHERE id = ?",
			rewardID,
		).Scan(&playerID, &questID, &currentRedeemed)
		if err != nil {
			if err == sql.ErrNoRows {
				return fmt.Errorf("quest reward not found: %d", rewardID)
			}
			return fmt.Errorf("failed to check quest reward status: %w", err)
		}

		if currentRedeemed == 1 {
			return fmt.Errorf("quest reward already redeemed: %d", rewardID)
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

// GetQuestDefinition retrieves quest definition data to determine rewards
func GetQuestDefinition(questID uint32) *QuestDef {
	// TODO: This is a stub. In a real implementation, this would load quest definitions from a CSV or database file.
	// For now, return a simple definition for quest ID 1 (First Steps)
	if questID == 1 {
		return &QuestDef{
			ID:          1,
			Title:       "First Steps",
			RewardItemID: 33,
			RewardCount: 8,
			Era:         0,
			Section:     "getting_started",
		}
	}
	return nil
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
	ID          uint32
	Title       string
	RewardItemID uint16
	RewardCount  uint8
	Era          uint8
	Section     string
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