package main

import (
	"database/sql"
	"fmt"
)

type QuestProgress struct {
	PlayerID      uint64
	QuestID       uint32
	Status        uint8
	ProgressPercent uint8
}

// InitQuestProgressTable creates the quest_progress table if it doesn't exist.
func InitQuestProgressTable(db *sql.DB) error {
	schema := `CREATE TABLE IF NOT EXISTS quest_progress (
		player_id INTEGER NOT NULL,
		quest_id INTEGER NOT NULL,
		status INTEGER NOT NULL DEFAULT 0,
		progress_percent INTEGER NOT NULL DEFAULT 0,
		PRIMARY KEY (player_id, quest_id),
		FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE
	);
	CREATE INDEX IF NOT EXISTS idx_quest_progress_player ON quest_progress(player_id);`
	if _, err := db.Exec(schema); err != nil {
		return fmt.Errorf("failed to create quest_progress table: %w", err)
	}
	return nil
}

// GetQuestProgress loads all quest progress for a player.
func GetQuestProgress(db *sql.DB, playerID uint64) ([]QuestProgress, error) {
	query := "SELECT player_id, quest_id, status, progress_percent FROM quest_progress WHERE player_id = ? ORDER BY quest_id"
	rows, err := db.Query(query, playerID)
	if err != nil {
		return nil, fmt.Errorf("failed to query quest progress: %w", err)
	}
	defer rows.Close()

	var results []QuestProgress
	for rows.Next() {
		var qp QuestProgress
		err := rows.Scan(&qp.PlayerID, &qp.QuestID, &qp.Status, &qp.ProgressPercent)
		if err != nil {
			return nil, fmt.Errorf("failed to scan quest progress row: %w", err)
		}
		results = append(results, qp)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("error iterating quest progress rows: %w", err)
	}

	return results, nil
}

// SetQuestProgress upserts a single quest progress record for a player.
func SetQuestProgress(db *sql.DB, playerID uint64, questID uint32, status uint8, progressPercent uint8) error {
	_, err := db.Exec(
		"INSERT INTO quest_progress (player_id, quest_id, status, progress_percent) VALUES (?, ?, ?, ?)"+
			" ON CONFLICT(player_id, quest_id) DO UPDATE SET status=excluded.status, progress_percent=excluded.progress_percent",
		playerID, questID, status, progressPercent)
	return err
}

// SetQuestProgressBatch upserts multiple quest progress records for a player in a transaction.
func SetQuestProgressBatch(db *sql.DB, playerID uint64, progresses []QuestProgress) error {
	tx, err := db.Begin()
	if err != nil {
		return fmt.Errorf("failed to begin transaction: %w", err)
	}
	defer tx.Rollback()

	for _, qp := range progresses {
		_, err := tx.Exec(
			"INSERT INTO quest_progress (player_id, quest_id, status, progress_percent) VALUES (?, ?, ?, ?)"+
				" ON CONFLICT(player_id, quest_id) DO UPDATE SET status=excluded.status, progress_percent=excluded.progress_percent",
			qp.PlayerID, qp.QuestID, qp.Status, qp.ProgressPercent)
		if err != nil {
			return fmt.Errorf("failed to upsert quest %d: %w", qp.QuestID, err)
		}
	}

	return tx.Commit()
}

// GetQuestProgressCount returns the number of quest progress records for a player.
func GetQuestProgressCount(db *sql.DB, playerID uint64) (int, error) {
	var count int
	err := db.QueryRow("SELECT COUNT(*) FROM quest_progress WHERE player_id = ?", playerID).Scan(&count)
	if err != nil {
		return 0, fmt.Errorf("failed to count quest progress for player %d: %w", playerID, err)
	}
	return count, nil
}

// DeleteQuestProgress deletes all quest progress for a player.
func DeleteQuestProgress(db *sql.DB, playerID uint64) error {
	_, err := db.Exec("DELETE FROM quest_progress WHERE player_id = ?", playerID)
	if err != nil {
		return fmt.Errorf("failed to delete quest progress for player %d: %w", playerID, err)
	}
	return nil
}