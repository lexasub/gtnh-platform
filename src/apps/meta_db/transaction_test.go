// gp-7uy: unit tests for the SQLite transactional save path in src/apps/meta_db.
//
// The transaction boundary under test is CreateInventory (db.go) and
// SetQuestProgressBatch (quest_progress.go): both DELETE the old state and
// re-INSERT the new one inside a single db.Begin()..tx.Commit(), so a failure
// anywhere in the middle must leave the previous saved state completely intact
// — never a half-written player.
//
// These drive the real exported functions against a real SQLite file in
// t.TempDir(). Nothing here reimplements the save path, and no production
// behaviour is changed to make the assertions pass.
//
// Why it pays: the recent bug in this file class (queued frame payload
// aliasing, gp-3v5x) was found by a unit test, and the transactional claim in
// this file is currently unverified by anything.

package main

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// newTxnTestMetaDB builds a MetaDB backed by a temp-dir SQLite file and
// creates the quest_progress table the batch writer needs. The directory is
// removed by t.TempDir(); the real data directory is never touched.
func newTxnTestMetaDB(t *testing.T) *MetaDB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "gp-7uy.db")
	m, err := NewMetaDB(path)
	if err != nil {
		t.Fatalf("NewMetaDB(%s): %v", path, err)
	}
	if err := InitQuestProgressTable(m.db); err != nil {
		m.db.Close()
		t.Fatalf("InitQuestProgressTable: %v", err)
	}
	t.Cleanup(func() { m.db.Close() })
	return m
}

// slots builds n inventory slots with distinguishable values.
func slots(n int, base uint16) []SlotData {
	out := make([]SlotData, 0, n)
	for i := 0; i < n; i++ {
		out = append(out, SlotData{Slot: i, BlockID: base + uint16(i), Count: uint8(i%64 + 1)})
	}
	return out
}

// ---------------------------------------------------------------------------
// A successful save is immediately readable.
// ---------------------------------------------------------------------------

func TestCreateInventoryCommitIsImmediatelyReadable(t *testing.T) {
	m := newTxnTestMetaDB(t)

	if err := m.CreateInventory(1, slots(5, 100)); err != nil {
		t.Fatalf("CreateInventory: %v", err)
	}

	// Read back through the real getter, without any intervening flush.
	got, err := m.GetInventory(1)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	if len(got) != 5 {
		t.Fatalf("got %d slots, want 5", len(got))
	}
	for i, s := range got {
		if s.Slot != i {
			t.Errorf("slot %d: Slot=%d", i, s.Slot)
		}
		if s.BlockID != uint16(100+i) {
			t.Errorf("slot %d: BlockID=%d want %d", i, s.BlockID, 100+i)
		}
		if s.Count != uint8(i%64+1) {
			t.Errorf("slot %d: Count=%d want %d", i, s.Count, i%64+1)
		}
	}

	// A committed save also creates the player row, so the position upsert
	// that follows it in the logout path is not orphaned.
	if n := m.GetPlayerCount(); n != 1 {
		t.Errorf("GetPlayerCount=%d want 1 after a committed inventory save", n)
	}
}

// ---------------------------------------------------------------------------
// A successful save is durable across a process boundary: close the handle,
// reopen the same file, read it back.
// ---------------------------------------------------------------------------

func TestCreateInventorySurvivesReopen(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "gp-7uy.db")

	{
		m, err := NewMetaDB(path)
		if err != nil {
			t.Fatalf("NewMetaDB: %v", err)
		}
		if err := m.CreateInventory(7, slots(3, 200)); err != nil {
			m.db.Close()
			t.Fatalf("CreateInventory: %v", err)
		}
		if err := m.db.Close(); err != nil {
			t.Fatalf("Close: %v", err)
		}
	}

	{
		m, err := NewMetaDB(path)
		if err != nil {
			t.Fatalf("reopen NewMetaDB: %v", err)
		}
		defer m.db.Close()
		got, err := m.GetInventory(7)
		if err != nil {
			t.Fatalf("GetInventory after reopen: %v", err)
		}
		if len(got) != 3 {
			t.Fatalf("after reopen got %d slots, want 3", len(got))
		}
		for i, s := range got {
			if s.BlockID != uint16(200+i) {
				t.Errorf("after reopen slot %d: BlockID=%d want %d", i, s.BlockID, 200+i)
			}
		}
	}
}

// ---------------------------------------------------------------------------
// A failed transaction leaves no partial row.
//
// CreateInventory's structure is the thing under test: it does
// INSERT OR IGNORE players, DELETE FROM inventory, then a prepared INSERT per
// slot, and only then Commit(). Any error in the middle returns without
// committing, and the deferred tx.Rollback() must undo the DELETE. If the
// rollback were missing, a player who had a saved inventory would come back
// with an empty one — silent data loss on a failed logout.
// ---------------------------------------------------------------------------

// A SlotData with a block id too large for the column is not usable, so the
// failure is injected at the database level instead: a duplicate primary key
// inside one transaction makes the prepared INSERT fail partway through the
// slot loop, after the DELETE has already run.
func TestCreateInventoryFailureLeavesPreviousSaveIntact(t *testing.T) {
	m := newTxnTestMetaDB(t)

	// A good save first, so there is previous state to protect.
	good := []SlotData{
		{Slot: 0, BlockID: 11, Count: 5},
		{Slot: 1, BlockID: 22, Count: 6},
		{Slot: 2, BlockID: 23, Count: 7},
	}
	if err := m.CreateInventory(1, good); err != nil {
		t.Fatalf("seed CreateInventory: %v", err)
	}

	// A slot list that violates the (player_id, slot) primary key: the second
	// write to slot 0 fails, so the transaction aborts mid-loop with the
	// DELETE already applied.
	bad := []SlotData{
		{Slot: 0, BlockID: 99, Count: 1},
		{Slot: 1, BlockID: 98, Count: 1},
		{Slot: 0, BlockID: 97, Count: 1}, // duplicate slot 0 -> UNIQUE violation
	}
	if err := m.CreateInventory(1, bad); err == nil {
		t.Fatal("CreateInventory with a duplicate slot returned nil error; " +
			"expected the transaction to fail")
	}

	// The critical assertion: the failed save must not have left the player
	// empty. Either the old rows are all still there, or — if the driver
	// happened to abort before the DELETE — the table is consistent. What must
	// never happen is a partial overwrite.
	got, err := m.GetInventory(1)
	if err != nil {
		t.Fatalf("GetInventory after failed save: %v", err)
	}
	if len(got) != len(good) {
		t.Fatalf("after a failed save got %d slots, want %d — the failed "+
			"transaction was not rolled back", len(got), len(good))
	}
	for _, s := range got {
		want := good[s.Slot]
		if s.BlockID != want.BlockID || s.Count != want.Count {
			t.Errorf("slot %d after failed save: got block=%d count=%d want "+
				"block=%d count=%d", s.Slot, s.BlockID, s.Count, want.BlockID, want.Count)
		}
	}
}

// A zero-slot save is a legitimate "empty the inventory" operation, not an
// error, and it must not be confused with a failed transaction.
func TestCreateInventoryEmptySlotsClears(t *testing.T) {
	m := newTxnTestMetaDB(t)

	if err := m.CreateInventory(1, slots(4, 50)); err != nil {
		t.Fatalf("seed CreateInventory: %v", err)
	}
	if err := m.CreateInventory(1, nil); err != nil {
		t.Fatalf("CreateInventory(nil slots): %v", err)
	}
	got, err := m.GetInventory(1)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	if len(got) != 0 {
		t.Fatalf("got %d slots after an empty save, want 0", len(got))
	}
	// The player row itself survives an empty inventory save.
	if n := m.GetPlayerCount(); n != 1 {
		t.Errorf("GetPlayerCount=%d want 1, an empty save must not drop the player", n)
	}
}

// CreateInventory is a full replace: saving 2 slots after saving 4 must drop
// the old rows, not merge with them.
func TestCreateInventoryReplacesNotMerges(t *testing.T) {
	m := newTxnTestMetaDB(t)

	if err := m.CreateInventory(1, slots(4, 10)); err != nil {
		t.Fatalf("first CreateInventory: %v", err)
	}
	if err := m.CreateInventory(1, []SlotData{{Slot: 0, BlockID: 77, Count: 1}}); err != nil {
		t.Fatalf("second CreateInventory: %v", err)
	}
	got, err := m.GetInventory(1)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d slots after replace, want 1 (slots=%+v)", len(got), got)
	}
	if got[0].BlockID != 77 || got[0].Count != 1 {
		t.Errorf("got %+v want block=77 count=1", got[0])
	}
}

// ---------------------------------------------------------------------------
// The quest batch writer has the same transaction shape.
// ---------------------------------------------------------------------------

func TestSetQuestProgressBatchCommitIsImmediatelyReadable(t *testing.T) {
	m := newTxnTestMetaDB(t)
	if _, err := m.db.Exec("INSERT OR IGNORE INTO players (id) VALUES (?)", 5); err != nil {
		t.Fatalf("seed player: %v", err)
	}

	batch := []QuestProgress{
		{PlayerID: 5, QuestID: 1, Status: 1, ProgressPercent: 10},
		{PlayerID: 5, QuestID: 2, Status: 2, ProgressPercent: 50},
		{PlayerID: 5, QuestID: 3, Status: 0, ProgressPercent: 0},
	}
	if err := SetQuestProgressBatch(m.db, 5, batch); err != nil {
		t.Fatalf("SetQuestProgressBatch: %v", err)
	}

	got, err := GetQuestProgress(m.db, 5)
	if err != nil {
		t.Fatalf("GetQuestProgress: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("got %d quest rows, want 3", len(got))
	}
	for i, qp := range got {
		want := batch[i]
		if qp.QuestID != want.QuestID || qp.Status != want.Status ||
			qp.ProgressPercent != want.ProgressPercent {
			t.Errorf("row %d: got %+v want %+v", i, qp, want)
		}
	}
	if n, err := GetQuestProgressCount(m.db, 5); err != nil || n != 3 {
		t.Errorf("GetQuestProgressCount=%d err=%v want 3", n, err)
	}
}

// A batch containing one bad row must write NONE of them. This is the
// all-or-nothing property of the transaction, and it is what distinguishes a
// real transaction from a loop of independent statements.
//
// The failure is forced with a test-local BEFORE INSERT trigger rather than a
// foreign-key violation: SQLite only enforces FOREIGN KEY when
// `PRAGMA foreign_keys=ON`, which this driver does not set, so an FK-based
// test would silently pass on a broken implementation. The trigger lives only
// in this test's temp database and changes no production schema.
func TestSetQuestProgressBatchFailureWritesNothing(t *testing.T) {
	m := newTxnTestMetaDB(t)
	if _, err := m.db.Exec("INSERT OR IGNORE INTO players (id) VALUES (?)", 9); err != nil {
		t.Fatalf("seed player: %v", err)
	}

	// A committed batch first, so there is a previous state to protect.
	good := []QuestProgress{
		{PlayerID: 9, QuestID: 1, Status: 1, ProgressPercent: 25},
		{PlayerID: 9, QuestID: 2, Status: 1, ProgressPercent: 75},
	}
	if err := SetQuestProgressBatch(m.db, 9, good); err != nil {
		t.Fatalf("seed SetQuestProgressBatch: %v", err)
	}

	// Reject quest 4 at the storage layer. The batch below reaches quest 4
	// after already upserting 3 and 5, so the failure lands mid-loop with
	// earlier statements done inside the open transaction.
	if _, err := m.db.Exec(`
		CREATE TRIGGER gp_7uy_reject BEFORE INSERT ON quest_progress
		WHEN NEW.quest_id = 4
		BEGIN SELECT RAISE(ABORT, 'gp-7uy: rejected quest 4'); END;`); err != nil {
		t.Fatalf("install trigger: %v", err)
	}

	bad := []QuestProgress{
		{PlayerID: 9, QuestID: 3, Status: 1, ProgressPercent: 5},
		{PlayerID: 9, QuestID: 4, Status: 1, ProgressPercent: 5}, // aborts here
		{PlayerID: 9, QuestID: 5, Status: 1, ProgressPercent: 5},
	}
	if err := SetQuestProgressBatch(m.db, 9, bad); err == nil {
		t.Fatal("SetQuestProgressBatch with a rejected row returned nil error; " +
			"expected the transaction to fail")
	}

	// Nothing from the failed batch may be visible: not the rows written
	// before the failure, not the one after it.
	got, err := GetQuestProgress(m.db, 9)
	if err != nil {
		t.Fatalf("GetQuestProgress after failed batch: %v", err)
	}
	if len(got) != len(good) {
		t.Fatalf("after a failed batch got %d quest rows, want %d — a partial "+
			"batch was committed (rows=%+v)", len(got), len(good), got)
	}
	for _, qp := range got {
		if qp.QuestID == 3 || qp.QuestID == 4 || qp.QuestID == 5 {
			t.Errorf("quest %d leaked from the failed batch", qp.QuestID)
		}
	}
	// The previous batch is untouched, value for value.
	for i, qp := range got {
		if qp.QuestID != good[i].QuestID ||
			qp.Status != good[i].Status ||
			qp.ProgressPercent != good[i].ProgressPercent {
			t.Errorf("row %d was modified by the failed batch: got %+v want %+v",
				i, qp, good[i])
		}
	}

	// A follow-up good batch still works, i.e. the abort left no lock behind.
	if _, err := m.db.Exec("DROP TRIGGER gp_7uy_reject"); err != nil {
		t.Fatalf("drop trigger: %v", err)
	}
	if err := SetQuestProgressBatch(m.db, 9, []QuestProgress{
		{PlayerID: 9, QuestID: 6, Status: 1, ProgressPercent: 1},
	}); err != nil {
		t.Fatalf("write after a failed batch: %v", err)
	}
	if n, err := GetQuestProgressCount(m.db, 9); err != nil || n != 3 {
		t.Errorf("GetQuestProgressCount=%d err=%v want 3", n, err)
	}
}

// The batch upsert is idempotent: re-running the same batch must not duplicate
// rows or double-apply progress.
func TestSetQuestProgressBatchIsIdempotent(t *testing.T) {
	m := newTxnTestMetaDB(t)
	if _, err := m.db.Exec("INSERT OR IGNORE INTO players (id) VALUES (?)", 11); err != nil {
		t.Fatalf("seed player: %v", err)
	}
	batch := []QuestProgress{
		{PlayerID: 11, QuestID: 1, Status: 2, ProgressPercent: 30},
		{PlayerID: 11, QuestID: 2, Status: 2, ProgressPercent: 60},
	}
	for i := 0; i < 3; i++ {
		if err := SetQuestProgressBatch(m.db, 11, batch); err != nil {
			t.Fatalf("SetQuestProgressBatch run %d: %v", i, err)
		}
	}
	got, err := GetQuestProgress(m.db, 11)
	if err != nil {
		t.Fatalf("GetQuestProgress: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("got %d rows after 3 identical batches, want 2", len(got))
	}
	for i, qp := range got {
		if qp.Status != batch[i].Status || qp.ProgressPercent != batch[i].ProgressPercent {
			t.Errorf("row %d: got %+v want %+v", i, qp, batch[i])
		}
	}
}

// An empty batch is a no-op that still commits cleanly, and it must not
// create or delete anything.
func TestSetQuestProgressBatchEmptyIsNoOp(t *testing.T) {
	m := newTxnTestMetaDB(t)
	if _, err := m.db.Exec("INSERT OR IGNORE INTO players (id) VALUES (?)", 13); err != nil {
		t.Fatalf("seed player: %v", err)
	}
	if err := SetQuestProgressBatch(m.db, 13, nil); err != nil {
		t.Fatalf("SetQuestProgressBatch(nil): %v", err)
	}
	if n, err := GetQuestProgressCount(m.db, 13); err != nil || n != 0 {
		t.Errorf("GetQuestProgressCount=%d err=%v want 0", n, err)
	}
}

// ---------------------------------------------------------------------------
// Concurrent saves to the same player do not interleave partial state.
//
// CreateInventory deletes the old rows and re-inserts the new ones inside one
// transaction. Two savers running against the same player must never leave a
// table holding a mix of both, and the database must not deadlock.
//
// SQLite serializes writers, so a correct implementation may legitimately
// report SQLITE_BUSY to a loser. What is NOT acceptable is a successful save
// that leaves partial state, so the invariant is stated over outcomes:
//   - every CreateInventory call either succeeds or returns an error,
//   - after every call has returned, the player's rows are exactly one
//     complete save set — never a blend of two,
//   - the per-player state is stable once the writers are done.
// ---------------------------------------------------------------------------

func TestConcurrentCreateInventoryNeverInterleaves(t *testing.T) {
	m := newTxnTestMetaDB(t)

	const (
		players = 4
		rounds  = 10
		slotsA  = 8
		slotsB  = 5
	)

	// Each payload is a distinct, self-consistent block of slots, so a blended
	// result is unambiguous.
	payloadA := slots(slotsA, 1000)
	payloadB := slots(slotsB, 2000)

	// validSet reports whether got is exactly one of the two payloads.
	validSet := func(got []SlotData) bool {
		for _, want := range [][]SlotData{payloadA, payloadB} {
			if len(got) != len(want) {
				continue
			}
			ok := true
			for i, s := range got {
				if s.Slot != want[i].Slot || s.BlockID != want[i].BlockID {
					ok = false
					break
				}
			}
			if ok {
				return true
			}
		}
		return false
	}

	// Two savers race on the SAME player. Their payloads differ in length and
	// base, so a lost update or a torn write is visible.
	const contested = 1
	var wg sync.WaitGroup
	var mu sync.Mutex
	busy := 0
	other := []string{}

	for w := 0; w < 2; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			payload := payloadA
			if w == 1 {
				payload = payloadB
			}
			for r := 0; r < rounds; r++ {
				if err := m.CreateInventory(contested, payload); err != nil {
					mu.Lock()
					if strings.Contains(err.Error(), "locked") ||
						strings.Contains(err.Error(), "busy") {
						busy++
					} else {
						other = append(other, err.Error())
					}
					mu.Unlock()
				}
			}
		}(w)
	}

	// Independent players saved at the same time: these must all succeed,
	// because no two of them contend for the same rows.
	for pl := 2; pl <= players; pl++ {
		wg.Add(1)
		go func(pl int) {
			defer wg.Done()
			for r := 0; r < rounds; r++ {
				if err := m.CreateInventory(uint64(pl), payloadA); err != nil {
					mu.Lock()
					other = append(other, fmt.Sprintf("player %d: %v", pl, err))
					mu.Unlock()
					return
				}
			}
		}(pl)
	}

	wg.Wait()

	// A "database is locked" failure is a correct answer for a losing writer.
	// Anything else is not.
	for _, e := range other {
		t.Errorf("unexpected save error: %s", e)
	}
	if busy > 0 {
		t.Logf("%d/%d contested saves returned SQLITE_BUSY (a legal outcome "+
			"under SQLite's writer lock)", busy, 2*rounds)
	}

	// The contested player must hold one complete set, not a blend.
	got, err := m.GetInventory(contested)
	if err != nil {
		t.Fatalf("GetInventory(%d): %v", contested, err)
	}
	if !validSet(got) {
		t.Errorf("contested player ended in a torn state: %+v", got)
	}

	// The uncontended players must each hold the full payload.
	for pl := 2; pl <= players; pl++ {
		got, err := m.GetInventory(uint64(pl))
		if err != nil {
			t.Fatalf("GetInventory(%d): %v", pl, err)
		}
		if len(got) != slotsA {
			t.Errorf("player %d: got %d slots, want %d", pl, len(got), slotsA)
			continue
		}
		for i, s := range got {
			if s.BlockID != payloadA[i].BlockID {
				t.Errorf("player %d slot %d: got %d want %d",
					pl, i, s.BlockID, payloadA[i].BlockID)
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The whole save is one transaction: an open transaction that is never
// committed must leave nothing behind, and the next writer must not be blocked
// by the abandoned one.
// ---------------------------------------------------------------------------

func TestRolledBackTransactionLeavesNoRows(t *testing.T) {
	m := newTxnTestMetaDB(t)
	if _, err := m.db.Exec("INSERT OR IGNORE INTO players (id) VALUES (?)", 21); err != nil {
		t.Fatalf("seed player: %v", err)
	}

	// Open a transaction, write, and roll it back explicitly — the same
	// teardown CreateInventory's deferred Rollback performs on error.
	tx, err := m.db.Begin()
	if err != nil {
		t.Fatalf("Begin: %v", err)
	}
	if _, err := tx.Exec(
		"INSERT INTO quest_progress (player_id, quest_id, status, progress_percent) VALUES (?,?,?,?)",
		21, 99, 1, 50); err != nil {
		tx.Rollback()
		t.Fatalf("insert inside tx: %v", err)
	}
	if err := tx.Rollback(); err != nil {
		t.Fatalf("Rollback: %v", err)
	}

	if n, err := GetQuestProgressCount(m.db, 21); err != nil || n != 0 {
		t.Errorf("after rollback GetQuestProgressCount=%d err=%v want 0", n, err)
	}
	// The next real write still works, i.e. no lock was left behind.
	if err := SetQuestProgressBatch(m.db, 21, []QuestProgress{
		{PlayerID: 21, QuestID: 1, Status: 1, ProgressPercent: 5},
	}); err != nil {
		t.Fatalf("write after rollback: %v", err)
	}
	if n, err := GetQuestProgressCount(m.db, 21); err != nil || n != 1 {
		t.Errorf("after post-rollback write GetQuestProgressCount=%d err=%v want 1", n, err)
	}
}

// ---------------------------------------------------------------------------
// Position save (the other half of a logout save) is immediately readable and
// upserts rather than duplicating.
// ---------------------------------------------------------------------------

func TestSavePlayerPositionUpserts(t *testing.T) {
	m := newTxnTestMetaDB(t)

	if err := m.SavePlayerPosition(3, 10, 64, -3); err != nil {
		t.Fatalf("SavePlayerPosition: %v", err)
	}
	p, err := m.GetPlayerPosition(3)
	if err != nil {
		t.Fatalf("GetPlayerPosition: %v", err)
	}
	if p.X != 10 || p.Y != 64 || p.Z != -3 {
		t.Errorf("got %+v want {10 64 -3}", p)
	}

	// Second save must overwrite, not insert a duplicate row.
	if err := m.SavePlayerPosition(3, 20, 65, 0); err != nil {
		t.Fatalf("second SavePlayerPosition: %v", err)
	}
	p, err = m.GetPlayerPosition(3)
	if err != nil {
		t.Fatalf("GetPlayerPosition: %v", err)
	}
	if p.X != 20 || p.Y != 65 || p.Z != 0 {
		t.Errorf("after upsert got %+v want {20 65 0}", p)
	}
	if n := m.GetPlayerCount(); n != 1 {
		t.Errorf("GetPlayerCount=%d want 1, the upsert duplicated the player row", n)
	}
}

// The full logout save — inventory plus position — must be consistent when
// read back, which is what the "logout" action in handleRequest does.
func TestLogoutSaveInventoryAndPositionAreConsistent(t *testing.T) {
	m := newTxnTestMetaDB(t)

	resp := handleRequest(m, Request{
		Action:   "logout",
		PlayerID: 31,
		Data: map[string]interface{}{
			"slots": []interface{}{
				map[string]interface{}{"slot": float64(0), "block_id": float64(4001), "count": float64(3)},
				map[string]interface{}{"slot": float64(1), "block_id": float64(4002), "count": float64(4)},
				map[string]interface{}{"slot": float64(2), "block_id": float64(4003), "count": float64(5)},
			},
			"x": float64(100),
			"y": float64(70),
			"z": float64(-20),
		},
	})
	if !resp.Success {
		t.Fatalf("logout failed: %+v", resp)
	}

	got, err := m.GetInventory(31)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("got %d slots, want 3", len(got))
	}
	for i, s := range got {
		if s.BlockID != uint16(4001+i) || s.Count != uint8(3+i) {
			t.Errorf("slot %d: got block=%d count=%d", i, s.BlockID, s.Count)
		}
	}
	pos, err := m.GetPlayerPosition(31)
	if err != nil {
		t.Fatalf("GetPlayerPosition: %v", err)
	}
	if pos.X != 100 || pos.Y != 70 || pos.Z != -20 {
		t.Errorf("position got %+v want {100 70 -20}", pos)
	}
}

// ---------------------------------------------------------------------------
// Hermeticity: the tests must never touch the real data directory.
// ---------------------------------------------------------------------------

func TestSaveDoesNotTouchCwdDatabase(t *testing.T) {
	// The repository ships a metadb.sqlite next to the sources. If any test
	// ever opens a relative path, that file is modified in place. This test
	// pins the expectation that the helpers use absolute temp-dir paths.
	if _, err := os.Stat("metadb.sqlite"); err == nil {
		before, err := os.Stat("metadb.sqlite")
		if err != nil {
			t.Fatalf("stat metadb.sqlite: %v", err)
		}
		m := newTxnTestMetaDB(t)
		if err := m.CreateInventory(1, slots(2, 10)); err != nil {
			t.Fatalf("CreateInventory: %v", err)
		}
		after, err := os.Stat("metadb.sqlite")
		if err != nil {
			t.Fatalf("stat metadb.sqlite: %v", err)
		}
		if before.Size() != after.Size() || !before.ModTime().Equal(after.ModTime()) {
			t.Errorf("metadb.sqlite was modified during a test: %d -> %d bytes",
				before.Size(), after.Size())
		}
	}
}

// A compile-time reminder that the fixtures really do use *sql.DB, so a
// future refactor of MetaDB that hides the handle is caught at build time.
var _ = (*sql.DB)(nil)
