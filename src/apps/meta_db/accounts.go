package main

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"math"
	"strings"
	"time"

	"github.com/gtnh/platform/gtnh-common/auth"
)

var (
	ErrAccountExists     = errors.New("account already exists")
	ErrInvalidCredential = errors.New("invalid credentials")
)

const (
	registrationAttempts   = 4
	registrationRetryDelay = 15 * time.Millisecond
)

// Account is the stable account identity and its existing numeric gameplay key.
// Password hashes are deliberately never exposed through this type.
type Account struct {
	ID       string
	Username string
	PlayerID int64
}

// seedPlayerIDAllocator advances the AUTOINCREMENT sequence beyond every
// existing player profile without associating any of those rows with accounts.
func (m *MetaDB) seedPlayerIDAllocator(ctx context.Context) error {
	tx, err := m.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin player ID allocator seed: %w", err)
	}
	defer tx.Rollback()
	if err := seedPlayerIDAllocatorTx(ctx, tx); err != nil {
		return err
	}
	return tx.Commit()
}

func seedPlayerIDAllocatorTx(ctx context.Context, tx *sql.Tx) error {
	var maxPlayerID int64
	if err := tx.QueryRowContext(ctx, "SELECT COALESCE(MAX(id), 0) FROM players").Scan(&maxPlayerID); err != nil {
		return fmt.Errorf("read legacy player ID high-water mark: %w", err)
	}
	if maxPlayerID < 0 {
		return fmt.Errorf("legacy player ID high-water mark is negative: %d", maxPlayerID)
	}
	if maxPlayerID == math.MaxInt64 {
		return errors.New("player ID space exhausted")
	}
	if maxPlayerID == 0 {
		return nil
	}
	if _, err := tx.ExecContext(ctx, "INSERT OR IGNORE INTO player_id_allocator(id) VALUES (?)", maxPlayerID); err != nil {
		return fmt.Errorf("seed player ID high-water mark: %w", err)
	}
	return nil
}

// RegisterAccount stores a canonical username and encoded hash, then commits
// the account, its player profile, and their mapping in one transaction.
func (m *MetaDB) RegisterAccount(ctx context.Context, username, passwordHash string) (Account, error) {
	canonical, err := auth.CanonicalUsername(username)
	if err != nil {
		return Account{}, err
	}
	if err := auth.ValidatePasswordHash(passwordHash); err != nil {
		return Account{}, fmt.Errorf("invalid password hash: %w", err)
	}

	for attempt := 0; attempt < registrationAttempts; attempt++ {
		account, err := m.registerAccountOnce(ctx, canonical, passwordHash)
		if err == nil || !isSQLiteBusy(err) || attempt+1 == registrationAttempts {
			return account, err
		}
		timer := time.NewTimer(registrationRetryDelay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return Account{}, ctx.Err()
		case <-timer.C:
		}
	}
	return Account{}, errors.New("account registration retry limit exceeded")
}

func (m *MetaDB) registerAccountOnce(ctx context.Context, username, passwordHash string) (Account, error) {
	accountID, err := newAccountID()
	if err != nil {
		return Account{}, err
	}
	tx, err := m.db.BeginTx(ctx, nil)
	if err != nil {
		return Account{}, fmt.Errorf("begin account registration: %w", err)
	}
	defer tx.Rollback()

	// Reseed under SQLite's transaction/write lock because legacy game paths can
	// create numeric player rows without account registrations.
	if err := seedPlayerIDAllocatorTx(ctx, tx); err != nil {
		return Account{}, err
	}
	result, err := tx.ExecContext(ctx, "INSERT INTO player_id_allocator DEFAULT VALUES")
	if err != nil {
		return Account{}, fmt.Errorf("allocate player ID: %w", err)
	}
	playerID, err := result.LastInsertId()
	if err != nil {
		return Account{}, fmt.Errorf("read allocated player ID: %w", err)
	}
	if playerID <= 0 {
		return Account{}, errors.New("player ID space exhausted")
	}
	if _, err := tx.ExecContext(ctx, "INSERT INTO players (id) VALUES (?)", playerID); err != nil {
		return Account{}, fmt.Errorf("create player profile: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO accounts (account_id, username, password_hash) VALUES (?, ?, ?)`,
		accountID, username, passwordHash); err != nil {
		if isUniqueConstraintError(err) {
			return Account{}, ErrAccountExists
		}
		return Account{}, fmt.Errorf("create account: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO account_player_map (account_id, player_id) VALUES (?, ?)`,
		accountID, playerID); err != nil {
		return Account{}, fmt.Errorf("map account to player: %w", err)
	}
	if err := tx.Commit(); err != nil {
		if isUniqueConstraintError(err) {
			return Account{}, ErrAccountExists
		}
		return Account{}, fmt.Errorf("commit account registration: %w", err)
	}
	return Account{ID: accountID, Username: username, PlayerID: playerID}, nil
}

// Valid-format fixed dummy hash so unknown-account login runs Argon2id without
// paying a separate password-hash KDF to prepare a placeholder on each miss.
const dummyCredentialHash = "$argon2id$v=19$m=65536,t=3,p=1$MDEyMzQ1Njc4OWFiY2RlZg$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

// AuthenticateAccount performs a uniform username/password login lookup.
// Unknown usernames and wrong passwords both return ErrInvalidCredential.
func (m *MetaDB) AuthenticateAccount(ctx context.Context, username, password string, hasher *auth.PasswordHasher) (Account, error) {
	canonical, err := auth.CanonicalUsername(username)
	if err != nil || auth.ValidatePassword(password) != nil {
		return Account{}, ErrInvalidCredential
	}

	var account Account
	var encodedHash string
	err = m.db.QueryRowContext(ctx, `
		SELECT a.account_id, a.username, mapping.player_id, a.password_hash
		FROM accounts AS a
		JOIN account_player_map AS mapping ON mapping.account_id = a.account_id
		WHERE a.username = ?`, canonical).Scan(&account.ID, &account.Username, &account.PlayerID, &encodedHash)
	accountFound := !errors.Is(err, sql.ErrNoRows)
	if errors.Is(err, sql.ErrNoRows) {
		// The fixed, valid-format dummy hash preserves the Argon2 verification
		// cost for unknown usernames without running another KDF on each miss.
		encodedHash = dummyCredentialHash
	} else if err != nil {
		return Account{}, fmt.Errorf("lookup account: %w", err)
	}

	ok, err := hasher.VerifyPassword(ctx, encodedHash, password)
	if err != nil {
		return Account{}, fmt.Errorf("verify stored password hash: %w", err)
	}
	if !ok || !accountFound {
		return Account{}, ErrInvalidCredential
	}
	return account, nil
}

func newAccountID() (string, error) {
	var bytes [16]byte
	if _, err := rand.Read(bytes[:]); err != nil {
		return "", fmt.Errorf("generate account ID: %w", err)
	}
	return hex.EncodeToString(bytes[:]), nil
}

func isUniqueConstraintError(err error) bool {
	if err == nil {
		return false
	}
	lower := strings.ToLower(err.Error())
	return strings.Contains(lower, "unique constraint") || strings.Contains(lower, "primary key")
}

func isSQLiteBusy(err error) bool {
	if err == nil {
		return false
	}
	lower := strings.ToLower(err.Error())
	return strings.Contains(lower, "database is locked") || strings.Contains(lower, "database is busy")
}
