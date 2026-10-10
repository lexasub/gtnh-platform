// Package auth provides credential validation and password hashing helpers.
// Account persistence and player/gameplay policy belong to the owning service.
package auth

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"

	"golang.org/x/crypto/argon2"
)

const (
	MinUsernameBytes = 3
	MaxUsernameBytes = 32
	MinPasswordBytes = 8
	MaxPasswordBytes = 128

	argonVersion     = 19 // Argon2 version 1.3
	argonTime        = uint32(3)
	argonMemoryKiB   = uint32(65536)
	argonParallelism = uint8(1)
	saltLength       = 16
	keyLength        = 32

	maxStoredTime        = uint32(6)
	maxStoredMemoryKiB   = uint32(262144)
	maxStoredParallelism = uint8(4)
	minStoredSaltLength  = 16
	maxStoredSaltLength  = 64
	minStoredKeyLength   = 32
	maxStoredKeyLength   = 64
	maxEncodedHashLength = 512
)

var (
	ErrInvalidUsername = errors.New("invalid username")
	ErrInvalidPassword = errors.New("invalid password")
	ErrMalformedHash   = errors.New("malformed password hash")
	ErrUnsafeHash      = errors.New("password hash parameters exceed safety bounds")
)

type Parameters struct {
	Time        uint32
	MemoryKiB   uint32
	Parallelism uint8
	SaltLength  int
	KeyLength   uint32
}

func DefaultParameters() Parameters {
	return Parameters{
		Time:        argonTime,
		MemoryKiB:   argonMemoryKiB,
		Parallelism: argonParallelism,
		SaltLength:  saltLength,
		KeyLength:   keyLength,
	}
}

// CanonicalUsername applies the account username contract identically before
// account registration and lookup: reject invalid UTF-8, trim edge ASCII space
// and horizontal tab, lowercase ASCII capitals, then enforce [a-z0-9_].
func CanonicalUsername(username string) (string, error) {
	if !utf8.ValidString(username) {
		return "", ErrInvalidUsername
	}

	start, end := 0, len(username)
	for start < end && isEdgeTrimByte(username[start]) {
		start++
	}
	for end > start && isEdgeTrimByte(username[end-1]) {
		end--
	}
	canonical := []byte(username[start:end])
	if len(canonical) < MinUsernameBytes || len(canonical) > MaxUsernameBytes {
		return "", ErrInvalidUsername
	}
	for i, b := range canonical {
		switch {
		case b >= 'A' && b <= 'Z':
			canonical[i] = b + ('a' - 'A')
		case b >= 'a' && b <= 'z', b >= '0' && b <= '9', b == '_':
		default:
			return "", ErrInvalidUsername
		}
	}
	return string(canonical), nil
}

func isEdgeTrimByte(b byte) bool {
	return b == ' ' || b == '\t'
}

// ValidatePassword enforces the byte-length and UTF-8 contract before KDF work.
func ValidatePassword(password string) error {
	if !utf8.ValidString(password) || len(password) < MinPasswordBytes || len(password) > MaxPasswordBytes {
		return ErrInvalidPassword
	}
	return nil
}

type encodedHash struct {
	version     int
	time        uint32
	memoryKiB   uint32
	parallelism uint8
	salt        []byte
	key         []byte
}

// PasswordHasher bounds expensive Argon2 work for one service instance. Create
// one shared PasswordHasher for a MetaDB process rather than one per request.
type PasswordHasher struct {
	params Parameters
	work   chan struct{}
}

func NewPasswordHasher(maxConcurrent int) (*PasswordHasher, error) {
	if maxConcurrent < 1 {
		return nil, fmt.Errorf("max concurrent Argon2 operations must be positive")
	}
	return &PasswordHasher{
		params: DefaultParameters(),
		work:   make(chan struct{}, maxConcurrent),
	}, nil
}

// HashPassword hashes a password using the canonical Argon2id v1.3 parameters.
// Context can cancel queued work; Argon2 itself is not interruptible once begun.
func (h *PasswordHasher) HashPassword(ctx context.Context, password string) (string, error) {
	if err := ValidatePassword(password); err != nil {
		return "", err
	}
	if err := h.acquire(ctx); err != nil {
		return "", err
	}
	defer h.release()

	salt := make([]byte, h.params.SaltLength)
	if _, err := rand.Read(salt); err != nil {
		return "", fmt.Errorf("generate password salt: %w", err)
	}
	key := argon2.IDKey([]byte(password), salt, h.params.Time, h.params.MemoryKiB,
		h.params.Parallelism, h.params.KeyLength)
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argonVersion, h.params.MemoryKiB, h.params.Time, h.params.Parallelism,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(key)), nil
}

// VerifyPassword validates stored parameters before running Argon2, preventing
// database content from requesting unbounded CPU or memory. A malformed or
// unsafe hash is an internal data error, not a credential mismatch.
func (h *PasswordHasher) VerifyPassword(ctx context.Context, encoded, password string) (bool, error) {
	if err := ValidatePassword(password); err != nil {
		return false, err
	}
	parsed, err := parseEncodedHash(encoded)
	if err != nil {
		return false, err
	}
	if err := h.acquire(ctx); err != nil {
		return false, err
	}
	defer h.release()

	actual := argon2.IDKey([]byte(password), parsed.salt, parsed.time,
		parsed.memoryKiB, parsed.parallelism, uint32(len(parsed.key)))
	return subtle.ConstantTimeCompare(actual, parsed.key) == 1, nil
}

func (h *PasswordHasher) acquire(ctx context.Context) error {
	select {
	case h.work <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (h *PasswordHasher) release() { <-h.work }

// ValidatePasswordHash checks the encoded hash format and resource parameters
// without performing the expensive password verification operation.
func ValidatePasswordHash(encoded string) error {
	_, err := parseEncodedHash(encoded)
	return err
}

func parseEncodedHash(encoded string) (encodedHash, error) {
	if len(encoded) > maxEncodedHashLength {
		return encodedHash{}, ErrUnsafeHash
	}
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[0] != "" || parts[1] != "argon2id" {
		return encodedHash{}, ErrMalformedHash
	}
	var parsed encodedHash
	if !strings.HasPrefix(parts[2], "v=") {
		return encodedHash{}, ErrMalformedHash
	}
	version, err := strconv.Atoi(strings.TrimPrefix(parts[2], "v="))
	if err != nil {
		return encodedHash{}, ErrMalformedHash
	}
	parsed.version = version
	if parsed.version != argonVersion {
		return encodedHash{}, ErrUnsafeHash
	}
	params := strings.Split(parts[3], ",")
	if len(params) != 3 || !strings.HasPrefix(params[0], "m=") ||
		!strings.HasPrefix(params[1], "t=") || !strings.HasPrefix(params[2], "p=") {
		return encodedHash{}, ErrMalformedHash
	}
	memoryKiB, errMemory := strconv.ParseUint(strings.TrimPrefix(params[0], "m="), 10, 32)
	timeCost, errTime := strconv.ParseUint(strings.TrimPrefix(params[1], "t="), 10, 32)
	parallelism, errParallelism := strconv.ParseUint(strings.TrimPrefix(params[2], "p="), 10, 8)
	if errMemory != nil || errTime != nil || errParallelism != nil {
		return encodedHash{}, ErrMalformedHash
	}
	parsed.memoryKiB = uint32(memoryKiB)
	parsed.time = uint32(timeCost)
	parsed.parallelism = uint8(parallelism)
	if parsed.time < 1 || parsed.time > maxStoredTime || parsed.memoryKiB < 8*1024 || parsed.memoryKiB > maxStoredMemoryKiB ||
		parsed.parallelism < 1 || parsed.parallelism > maxStoredParallelism {
		return encodedHash{}, ErrUnsafeHash
	}
	var decodeErr error
	parsed.salt, decodeErr = base64.RawStdEncoding.DecodeString(parts[4])
	if decodeErr != nil || len(parsed.salt) < minStoredSaltLength || len(parsed.salt) > maxStoredSaltLength {
		return encodedHash{}, ErrMalformedHash
	}
	parsed.key, decodeErr = base64.RawStdEncoding.DecodeString(parts[5])
	if decodeErr != nil || len(parsed.key) < minStoredKeyLength || len(parsed.key) > maxStoredKeyLength {
		return encodedHash{}, ErrMalformedHash
	}
	return parsed, nil
}
