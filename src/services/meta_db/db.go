package main

import (
	"database/sql"
	"fmt"
	"log"

	flatbuffers "github.com/google/flatbuffers/go"
	_ "github.com/mattn/go-sqlite3"
	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// ---------------------------------------------------------------------------
// MetaDB core — SQLite persistence layer
// ---------------------------------------------------------------------------

const schema = `
CREATE TABLE IF NOT EXISTS players (
	id INTEGER PRIMARY KEY,
	x INTEGER NOT NULL DEFAULT 0,
	y INTEGER NOT NULL DEFAULT 0,
	z INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS inventory (
	player_id INTEGER NOT NULL,
	slot INTEGER NOT NULL,
	block_id INTEGER NOT NULL,
	count INTEGER NOT NULL DEFAULT 1,
	PRIMARY KEY (player_id, slot),
	FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_inventory_player ON inventory(player_id);

CREATE TABLE IF NOT EXISTS quest_progress (
	player_id INTEGER NOT NULL,
	quest_id INTEGER NOT NULL,
	status INTEGER NOT NULL DEFAULT 0,
	progress_percent INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (player_id, quest_id),
	FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_quest_progress_player ON quest_progress(player_id);

CREATE TABLE IF NOT EXISTS player_quest_rewards (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	player_id INTEGER NOT NULL,
	quest_id INTEGER NOT NULL,
	reward_type TEXT NOT NULL,
	reward_id INTEGER NOT NULL,
	reward_count INTEGER NOT NULL,
	reward_value REAL DEFAULT 0.0,
	redeemed INTEGER NOT NULL DEFAULT 0,
	reward_timestamp INTEGER NOT NULL,
	metadata TEXT,
	FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE,
	FOREIGN KEY (quest_id) REFERENCES quest_progress(quest_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_player_quest_rewards_player ON player_quest_rewards(player_id);
CREATE INDEX IF NOT EXISTS idx_player_quest_rewards_quest ON player_quest_rewards(quest_id);
CREATE INDEX IF NOT EXISTS idx_player_quest_rewards_redeemed ON player_quest_rewards(redeemed);
`

// MetaDB holds the SQLite connection and router reference.
type MetaDB struct {
	db *sql.DB
	rc *RouterClient
}

func (m *MetaDB) SetRouterClient(rc *RouterClient) {
	m.rc = rc
}

func NewMetaDB(dbPath string) (*MetaDB, error) {
	db, err := sql.Open("sqlite3", dbPath)
	if err != nil {
		return nil, fmt.Errorf("failed to open database: %w", err)
	}
	if err := db.Ping(); err != nil {
		return nil, fmt.Errorf("failed to ping database: %w", err)
	}

	m := &MetaDB{db: db}
	if err := m.initSchema(); err != nil {
		return nil, fmt.Errorf("failed to initialize schema: %w", err)
	}
	return m, nil
}

func (m *MetaDB) initSchema() error {
	_, err := m.db.Exec(schema)
	return err
}

// ---------------------------------------------------------------------------
// Inventory CRUD
// ---------------------------------------------------------------------------

func (m *MetaDB) CreateInventory(playerID uint64, slots []Protocol.InventorySlot) error {
	tx, err := m.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	if _, err = tx.Exec("INSERT OR IGNORE INTO players (id) VALUES (?)", playerID); err != nil {
		return err
	}
	if _, err = tx.Exec("DELETE FROM inventory WHERE player_id = ?", playerID); err != nil {
		return err
	}

	stmt, err := tx.Prepare("INSERT INTO inventory (player_id, slot, block_id, count) VALUES (?, ?, ?, ?)")
	if err != nil {
		return err
	}
	defer stmt.Close()

	for _, slot := range slots {
		if _, err = stmt.Exec(playerID, int(slot.ItemId()), int(slot.Count()), 0); err != nil {
			return err
		}
	}

	return tx.Commit()
}

func (m *MetaDB) GetInventorySlot(playerID uint64, slotNum int) (bool, int, int, error) {
	var blockID, count int
	err := m.db.QueryRow(
		"SELECT block_id, count FROM inventory WHERE player_id = ? AND slot = ?",
		playerID, slotNum,
	).Scan(&blockID, &count)
	if err == sql.ErrNoRows {
		return false, 0, 0, nil
	}
	if err != nil {
		return false, 0, 0, err
	}
	return true, blockID, count, nil
}

// SlotData holds parsed inventory slot from SQLite (avoids FlatBuffer round-trip).
type SlotData struct {
	Slot    int
	BlockID uint16
	Count   uint8
	Meta    uint16
}

func (m *MetaDB) GetInventory(playerID uint64) ([]SlotData, error) {
	rows, err := m.db.Query("SELECT slot, block_id, count FROM inventory WHERE player_id = ? ORDER BY slot", playerID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var result []SlotData
	for rows.Next() {
		var s SlotData
		blockID := 0
		count := 0
		if err := rows.Scan(&s.Slot, &blockID, &count); err != nil {
			return nil, err
		}
		s.BlockID = uint16(blockID)
		s.Count = uint8(count)
		result = append(result, s)
	}
	return result, nil
}

func (m *MetaDB) UpdateInventorySlot(playerID uint64, slotNum int, blockID int, count int) error {
	_, err := m.db.Exec(`
		INSERT INTO inventory (player_id, slot, block_id, count) 
		VALUES (?, ?, ?, ?)
		ON CONFLICT(player_id, slot) DO UPDATE SET 
			block_id = excluded.block_id,
			count = excluded.count
	`, playerID, slotNum, blockID, count)
	return err
}

func (m *MetaDB) DeleteInventorySlot(playerID uint64, slotNum int) error {
	_, err := m.db.Exec("DELETE FROM inventory WHERE player_id = ? AND slot = ?", playerID, slotNum)
	return err
}

func (m *MetaDB) DeleteInventory(playerID uint64) error {
	_, err := m.db.Exec("DELETE FROM inventory WHERE player_id = ?", playerID)
	return err
}

// PublishInventoryTo publishes inventory data to the given topic (for SimCore consumption).
// All 40 slots are serialized positionally — empty slots get item_id=0, count=0.
func (m *MetaDB) PublishInventoryTo(topic string, playerID uint64, slots []SlotData) {
	if m.rc == nil {
		return
	}
	log.Printf("[INV] PublishInventoryTo topic=%s player=%d slots=%d", topic, playerID, len(slots))

	// Build positionally-indexed array of 40 slots (kInventorySlots).
	type fbSlot struct{ itemID, count, meta uint16 }
	var allSlots [40]fbSlot
	for _, s := range slots {
		if s.Slot >= 0 && s.Slot < 40 {
			allSlots[s.Slot] = fbSlot{uint16(s.BlockID), uint16(s.Count), uint16(s.Meta)}
		}
	}

	builder := flatbuffers.NewBuilder(2048)
	slotOffsets := make([]flatbuffers.UOffsetT, 40)
	for i, s := range allSlots {
		Protocol.InventorySlotStart(builder)
		Protocol.InventorySlotAddItemId(builder, uint16(s.itemID))
		Protocol.InventorySlotAddCount(builder, uint8(s.count))
		Protocol.InventorySlotAddMeta(builder, uint16(s.meta))
		slotOffsets[i] = Protocol.InventorySlotEnd(builder)
	}

	Protocol.InventoryUpdateStartSlotsVector(builder, 40)
	for i := 39; i >= 0; i-- {
		builder.PrependUOffsetT(slotOffsets[i])
	}
	slotsVec := builder.EndVector(40)

	Protocol.InventoryUpdateStart(builder)
	Protocol.InventoryUpdateAddPlayerId(builder, playerID)
	Protocol.InventoryUpdateAddSlots(builder, slotsVec)
	updateOff := Protocol.InventoryUpdateEnd(builder)
	builder.Finish(updateOff)

	m.rc.PublishRaw(topic, builder.FinishedBytes())
}

// ---------------------------------------------------------------------------
// Player position CRUD
// ---------------------------------------------------------------------------

func (m *MetaDB) SavePlayerPosition(playerID uint64, x, y, z int) error {
	_, err := m.db.Exec(`
		INSERT INTO players (id, x, y, z) 
		VALUES (?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET 
			x = excluded.x,
			y = excluded.y,
			z = excluded.z
	`, playerID, x, y, z)
	return err
}

func (m *MetaDB) GetPlayerPosition(playerID uint64) (Player, error) {
	var p Player
	err := m.db.QueryRow("SELECT id, x, y, z FROM players WHERE id = ?", playerID).Scan(&p.ID, &p.X, &p.Y, &p.Z)
	return p, err
}

// ---------------------------------------------------------------------------
// Entity state CRUD
// ---------------------------------------------------------------------------

func (m *MetaDB) initEntityStateSchema() error {
	_, err := m.db.Exec(entityStateSchema)
	return err
}

func (m *MetaDB) GetEntityState(dim, x, y, z int) ([]byte, error) {
	var blob []byte
	err := m.db.QueryRow(
		"SELECT blob FROM entity_state WHERE dim = ? AND x = ? AND y = ? AND z = ?",
		dim, x, y, z,
	).Scan(&blob)
	if err != nil {
		return nil, err
	}
	return blob, nil
}

func (m *MetaDB) SetEntityState(dim, x, y, z int, blob []byte) error {
	_, err := m.db.Exec(
		`INSERT INTO entity_state (dim, x, y, z, blob) 
		 VALUES (?, ?, ?, ?, ?)
		 ON CONFLICT(dim, x, y, z) DO UPDATE SET blob = excluded.blob`,
		dim, x, y, z, blob,
	)
	return err
}

func (m *MetaDB) DeleteEntityState(dim, x, y, z int) error {
	result, err := m.db.Exec(
		"DELETE FROM entity_state WHERE dim = ? AND x = ? AND y = ? AND z = ?",
		dim, x, y, z,
	)
	if err != nil {
		return fmt.Errorf("failed to delete entity state: %w", err)
	}
	rows, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("failed to check rows affected: %w", err)
	}
	if rows == 0 {
		return fmt.Errorf("entity state not found at (%d,%d,%d) in dim %d", x, y, z, dim)
	}
	return nil
}

// GetPlayerCount returns the total number of players in the database.
func (m *MetaDB) GetPlayerCount() int {
	var count int
	err := m.db.QueryRow("SELECT COUNT(*) FROM players").Scan(&count)
	if err != nil {
		return 0
	}
	return count
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

type Player struct {
	ID uint64 `json:"id"`
	X  int    `json:"x"`
	Y  int    `json:"y"`
	Z  int    `json:"z"`
}

type InventorySlot struct {
	Slot    int
	BlockID int
	Count   int
}

// ---------------------------------------------------------------------------
// JSON handler (legacy TCP API — used by old clients)
// ---------------------------------------------------------------------------

type Request struct {
	Action   string      `json:"action"`
	PlayerID uint64      `json:"player_id"`
	Data     interface{} `json:"data"`
}

type Response struct {
	Success bool        `json:"success"`
	Error   string      `json:"error,omitempty"`
	Data    interface{} `json:"data,omitempty"`
}

func handleRequest(m *MetaDB, req Request) Response {
	switch req.Action {
	case "login":
		slots, err := m.GetInventory(req.PlayerID)
		if err != nil {
			return Response{Success: false, Error: err.Error()}
		}
		return Response{Success: true, Data: slots}

	case "logout":
		data, ok := req.Data.(map[string]interface{})
		if !ok {
			return Response{Success: false, Error: "invalid data format"}
		}

		slotsData, ok := data["slots"].([]interface{})
		if !ok {
			return Response{Success: false, Error: "invalid slots format"}
		}

		var slots []InventorySlot
		for _, s := range slotsData {
			slotMap, ok := s.(map[string]interface{})
			if !ok {
				continue
			}
			slots = append(slots, InventorySlot{
				Slot:    int(slotMap["slot"].(float64)),
				BlockID: int(slotMap["block_id"].(float64)),
				Count:   int(slotMap["count"].(float64)),
			})
		}

		protocolSlots := make([]Protocol.InventorySlot, len(slots))
		for i, slot := range slots {
			builder := flatbuffers.NewBuilder(0)
			Protocol.InventorySlotStart(builder)
			Protocol.InventorySlotAddItemId(builder, uint16(slot.BlockID))
			Protocol.InventorySlotAddCount(builder, uint8(slot.Count))
			Protocol.InventorySlotAddMeta(builder, 0)
			slotOffset := Protocol.InventorySlotEnd(builder)
			protocolSlots[i] = *Protocol.GetRootAsInventorySlot(builder.FinishedBytes(), slotOffset)
		}

		if err := m.CreateInventory(req.PlayerID, protocolSlots); err != nil {
			return Response{Success: false, Error: err.Error()}
		}

		if x, ok := data["x"].(float64); ok {
			if y, ok := data["y"].(float64); ok {
				if z, ok := data["z"].(float64); ok {
					m.SavePlayerPosition(req.PlayerID, int(x), int(y), int(z))
				}
			}
		}

		return Response{Success: true}

	case "update_slot":
		data, ok := req.Data.(map[string]interface{})
		if !ok {
			return Response{Success: false, Error: "invalid data format"}
		}
		slot := int(data["slot"].(float64))
		blockID := int(data["block_id"].(float64))
		count := int(data["count"].(float64))

		if err := m.UpdateInventorySlot(req.PlayerID, slot, blockID, count); err != nil {
			return Response{Success: false, Error: err.Error()}
		}
		return Response{Success: true}

	case "delete_slot":
		data, ok := req.Data.(map[string]interface{})
		if !ok {
			return Response{Success: false, Error: "invalid data format"}
		}
		slot := int(data["slot"].(float64))

		if err := m.DeleteInventorySlot(req.PlayerID, slot); err != nil {
			return Response{Success: false, Error: err.Error()}
		}
		return Response{Success: true}

	case "save_position":
		data, ok := req.Data.(map[string]interface{})
		if !ok {
			return Response{Success: false, Error: "invalid data format"}
		}
		x := int(data["x"].(float64))
		y := int(data["y"].(float64))
		z := int(data["z"].(float64))

		if err := m.SavePlayerPosition(req.PlayerID, x, y, z); err != nil {
			return Response{Success: false, Error: err.Error()}
		}
		return Response{Success: true}

	default:
		log.Printf("unknown action: %s", req.Action)
		return Response{Success: false, Error: "unknown action"}
	}
}
