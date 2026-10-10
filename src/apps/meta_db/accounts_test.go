package main

import (
	"context"
	"errors"
	"math"
	"path/filepath"
	"sync"
	"testing"

	"github.com/gtnh/platform/gtnh-common/auth"
)

func newAccountTestMetaDB(t *testing.T) *MetaDB {
	t.Helper()
	m, err := NewMetaDB(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatalf("NewMetaDB: %v", err)
	}
	t.Cleanup(func() { _ = m.db.Close() })
	return m
}

func testPasswordHash(t *testing.T) string {
	t.Helper()
	hasher, err := auth.NewPasswordHasher(2)
	if err != nil {
		t.Fatalf("NewPasswordHasher: %v", err)
	}
	hash, err := hasher.HashPassword(context.Background(), "correct-horse-battery")
	if err != nil {
		t.Fatalf("HashPassword: %v", err)
	}
	return hash
}

func TestRegisterAccountAllocatesAboveLegacyRows(t *testing.T) {
	m := newAccountTestMetaDB(t)
	for _, id := range []int64{4, 17, 9001} {
		if _, err := m.db.Exec("INSERT INTO players(id) VALUES (?)", id); err != nil {
			t.Fatalf("seed legacy player %d: %v", id, err)
		}
	}

	account, err := m.RegisterAccount(context.Background(), "player_one", testPasswordHash(t))
	if err != nil {
		t.Fatalf("RegisterAccount: %v", err)
	}
	if account.ID == "" || account.Username != "player_one" || account.PlayerID <= 9001 {
		t.Fatalf("unexpected registered account: %+v", account)
	}

	var mappedID int64
	if err := m.db.QueryRow("SELECT player_id FROM account_player_map WHERE account_id = ?", account.ID).Scan(&mappedID); err != nil {
		t.Fatalf("lookup account mapping: %v", err)
	}
	if mappedID != account.PlayerID {
		t.Fatalf("mapped player ID=%d, returned player ID=%d", mappedID, account.PlayerID)
	}
	var legacyMappings int
	if err := m.db.QueryRow("SELECT COUNT(*) FROM account_player_map WHERE player_id <= 9001").Scan(&legacyMappings); err != nil {
		t.Fatalf("count legacy mappings: %v", err)
	}
	if legacyMappings != 0 {
		t.Fatalf("registration claimed %d legacy player profiles", legacyMappings)
	}
}

func TestAuthenticateAccountReturnsUniformFailureForKnownAndUnknownUsers(t *testing.T) {
	m := newAccountTestMetaDB(t)
	hasher, err := auth.NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := m.RegisterAccount(context.Background(), "known_user", testPasswordHash(t)); err != nil {
		t.Fatalf("RegisterAccount: %v", err)
	}
	for _, username := range []string{"unknown_user", "known_user"} {
		if _, err := m.AuthenticateAccount(context.Background(), username,
			"incorrect-horse-battery", hasher); !errors.Is(err, ErrInvalidCredential) {
			t.Errorf("AuthenticateAccount(%q) error=%v, want ErrInvalidCredential", username, err)
		}
	}
	account, err := m.AuthenticateAccount(context.Background(), "KNOWN_USER",
		"correct-horse-battery", hasher)
	if err != nil || account.PlayerID == 0 {
		t.Fatalf("valid login account=%+v err=%v", account, err)
	}
}

func TestAccountAndPlayerDataSurviveMetaDBRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "accounts.db")
	m, err := NewMetaDB(path)
	if err != nil {
		t.Fatalf("NewMetaDB: %v", err)
	}
	first, err := m.RegisterAccount(context.Background(), "restart_user", testPasswordHash(t))
	if err != nil {
		m.db.Close()
		t.Fatalf("RegisterAccount: %v", err)
	}
	if err := m.UpdateInventorySlot(uint64(first.PlayerID), 3, 60948, 2); err != nil {
		m.db.Close()
		t.Fatalf("save inventory: %v", err)
	}
	if err := m.SavePlayerPosition(uint64(first.PlayerID), 12, 64, -9); err != nil {
		m.db.Close()
		t.Fatalf("save position: %v", err)
	}
	if err := m.db.Close(); err != nil {
		t.Fatalf("close MetaDB: %v", err)
	}

	m, err = NewMetaDB(path)
	if err != nil {
		t.Fatalf("reopen MetaDB: %v", err)
	}
	defer m.db.Close()
	hasher, err := auth.NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	account, err := m.AuthenticateAccount(context.Background(), "RESTART_USER", "correct-horse-battery", hasher)
	if err != nil {
		t.Fatalf("AuthenticateAccount after restart: %v", err)
	}
	if account.ID != first.ID || account.PlayerID != first.PlayerID {
		t.Fatalf("account identity changed across restart: before=%+v after=%+v", first, account)
	}
	found, blockID, count, err := m.GetInventorySlot(uint64(account.PlayerID), 3)
	if err != nil || !found || blockID != 60948 || count != 2 {
		t.Fatalf("restored inventory found=%t item=%d count=%d err=%v", found, blockID, count, err)
	}
	position, err := m.GetPlayerPosition(uint64(account.PlayerID))
	if err != nil || position.X != 12 || position.Y != 64 || position.Z != -9 {
		t.Fatalf("restored position=%+v err=%v", position, err)
	}
}

func TestRegisterAccountMappingSurvivesReopenAndDeletion(t *testing.T) {
	path := filepath.Join(t.TempDir(), "accounts.db")
	m, err := NewMetaDB(path)
	if err != nil {
		t.Fatalf("NewMetaDB: %v", err)
	}
	first, err := m.RegisterAccount(context.Background(), "persistent_user", testPasswordHash(t))
	if err != nil {
		m.db.Close()
		t.Fatalf("first RegisterAccount: %v", err)
	}
	if _, err := m.db.Exec("DELETE FROM account_player_map WHERE account_id = ?", first.ID); err != nil {
		m.db.Close()
		t.Fatalf("delete account mapping: %v", err)
	}
	if _, err := m.db.Exec("DELETE FROM accounts WHERE account_id = ?", first.ID); err != nil {
		m.db.Close()
		t.Fatalf("delete account: %v", err)
	}
	if _, err := m.db.Exec("DELETE FROM players WHERE id = ?", first.PlayerID); err != nil {
		m.db.Close()
		t.Fatalf("delete account player: %v", err)
	}
	if err := m.db.Close(); err != nil {
		t.Fatalf("close MetaDB: %v", err)
	}

	m, err = NewMetaDB(path)
	if err != nil {
		t.Fatalf("reopen MetaDB: %v", err)
	}
	defer m.db.Close()
	second, err := m.RegisterAccount(context.Background(), "persistent_user", testPasswordHash(t))
	if err != nil {
		t.Fatalf("second RegisterAccount: %v", err)
	}
	if second.PlayerID <= first.PlayerID {
		t.Fatalf("deleted account player ID was reused: first=%d second=%d", first.PlayerID, second.PlayerID)
	}
}

func TestRegisterAccountDuplicateUsernameRollsBackPlayerAndAllocator(t *testing.T) {
	m := newAccountTestMetaDB(t)
	first, err := m.RegisterAccount(context.Background(), "duplicate_user", testPasswordHash(t))
	if err != nil {
		t.Fatalf("first RegisterAccount: %v", err)
	}
	var playersBefore, accountsBefore int
	if err := m.db.QueryRow("SELECT COUNT(*) FROM players").Scan(&playersBefore); err != nil {
		t.Fatal(err)
	}
	if err := m.db.QueryRow("SELECT COUNT(*) FROM accounts").Scan(&accountsBefore); err != nil {
		t.Fatal(err)
	}
	if _, err := m.RegisterAccount(context.Background(), "duplicate_user", testPasswordHash(t)); !errors.Is(err, ErrAccountExists) {
		t.Fatalf("duplicate RegisterAccount error=%v, want ErrAccountExists", err)
	}
	var playersAfter, accountsAfter int
	_ = m.db.QueryRow("SELECT COUNT(*) FROM players").Scan(&playersAfter)
	_ = m.db.QueryRow("SELECT COUNT(*) FROM accounts").Scan(&accountsAfter)
	if playersAfter != playersBefore || accountsAfter != accountsBefore {
		t.Fatalf("duplicate registration left partial rows: players %d->%d accounts %d->%d", playersBefore, playersAfter, accountsBefore, accountsAfter)
	}
	var maxAllocated int64
	if err := m.db.QueryRow("SELECT seq FROM sqlite_sequence WHERE name = 'player_id_allocator'").Scan(&maxAllocated); err != nil {
		t.Fatalf("read allocator sequence: %v", err)
	}
	if maxAllocated != first.PlayerID {
		t.Fatalf("failed registration consumed allocator state: seq=%d first player=%d", maxAllocated, first.PlayerID)
	}
}

func TestRegisterAccountAllocatesLastPositiveIDs(t *testing.T) {
	m := newAccountTestMetaDB(t)
	if _, err := m.db.Exec("INSERT INTO players(id) VALUES (?)", int64(math.MaxInt64-2)); err != nil {
		t.Fatalf("seed near-maximum legacy player ID: %v", err)
	}
	first, err := m.RegisterAccount(context.Background(), "penultimate_id_user", testPasswordHash(t))
	if err != nil {
		t.Fatalf("allocate MaxInt64-1 player ID: %v", err)
	}
	if first.PlayerID != math.MaxInt64-1 {
		t.Fatalf("first allocated player ID=%d, want %d", first.PlayerID, int64(math.MaxInt64-1))
	}
	second, err := m.RegisterAccount(context.Background(), "last_id_user", testPasswordHash(t))
	if err != nil {
		t.Fatalf("allocate MaxInt64 player ID: %v", err)
	}
	if second.PlayerID != math.MaxInt64 {
		t.Fatalf("second allocated player ID=%d, want %d", second.PlayerID, int64(math.MaxInt64))
	}
	if _, err := m.RegisterAccount(context.Background(), "exhausted_id_user", testPasswordHash(t)); err == nil {
		t.Fatal("registration succeeded after exhausting signed 64-bit player IDs")
	}
}

func TestRegisterAccountRejectsOutOfRangePlayerIDs(t *testing.T) {
	m := newAccountTestMetaDB(t)
	if _, err := m.db.Exec("INSERT INTO players(id) VALUES (?)", int64(math.MaxInt64)); err != nil {
		t.Fatalf("seed maximum legacy player ID: %v", err)
	}
	if _, err := m.RegisterAccount(context.Background(), "overflow_user", testPasswordHash(t)); err == nil {
		t.Fatal("RegisterAccount succeeded after exhausting signed 64-bit player IDs")
	}
	var accounts, mappings int
	_ = m.db.QueryRow("SELECT COUNT(*) FROM accounts").Scan(&accounts)
	_ = m.db.QueryRow("SELECT COUNT(*) FROM account_player_map").Scan(&mappings)
	if accounts != 0 || mappings != 0 {
		t.Fatalf("ID exhaustion left partial auth rows: accounts=%d mappings=%d", accounts, mappings)
	}
}

func TestRegisterAccountConcurrentCanonicalUsernameIsUnique(t *testing.T) {
	m := newAccountTestMetaDB(t)
	const attempts = 8
	var wg sync.WaitGroup
	results := make(chan error, attempts)
	for i := 0; i < attempts; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := m.RegisterAccount(context.Background(), "race_user", testPasswordHash(t))
			results <- err
		}()
	}
	wg.Wait()
	close(results)
	var registered, duplicates int
	for err := range results {
		switch {
		case err == nil:
			registered++
		case errors.Is(err, ErrAccountExists):
			duplicates++
		default:
			t.Errorf("RegisterAccount returned unexpected error: %v", err)
		}
	}
	if registered != 1 || duplicates != attempts-1 {
		t.Fatalf("concurrent same-name registrations: success=%d duplicate=%d, want 1/%d", registered, duplicates, attempts-1)
	}
}
