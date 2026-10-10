package auth

import (
	"context"
	"errors"
	"strings"
	"testing"
)

func TestCanonicalUsername(t *testing.T) {
	tests := []struct {
		name    string
		input   string
		want    string
		wantErr bool
	}{
		{name: "trim and lowercase", input: " \tPlayer_1\t ", want: "player_1"},
		{name: "minimum length", input: "Ab1", want: "ab1"},
		{name: "maximum length", input: strings.Repeat("A", MaxUsernameBytes), want: strings.Repeat("a", MaxUsernameBytes)},
		{name: "too short", input: "ab", wantErr: true},
		{name: "too long", input: strings.Repeat("a", MaxUsernameBytes+1), wantErr: true},
		{name: "non ascii", input: "playér", wantErr: true},
		{name: "interior space", input: "player name", wantErr: true},
		{name: "punctuation", input: "player-name", wantErr: true},
		{name: "invalid utf8", input: string([]byte{0xff, 'a', 'b'}), wantErr: true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, err := CanonicalUsername(tc.input)
			if tc.wantErr {
				if !errors.Is(err, ErrInvalidUsername) {
					t.Fatalf("CanonicalUsername(%q) error=%v, want ErrInvalidUsername", tc.input, err)
				}
				return
			}
			if err != nil || got != tc.want {
				t.Fatalf("CanonicalUsername(%q)=(%q,%v), want %q", tc.input, got, err, tc.want)
			}
		})
	}
}

func TestValidatePassword(t *testing.T) {
	if err := ValidatePassword(strings.Repeat("p", MinPasswordBytes)); err != nil {
		t.Fatalf("minimum password rejected: %v", err)
	}
	if err := ValidatePassword(strings.Repeat("p", MaxPasswordBytes)); err != nil {
		t.Fatalf("maximum password rejected: %v", err)
	}
	for _, password := range []string{
		strings.Repeat("p", MinPasswordBytes-1),
		strings.Repeat("p", MaxPasswordBytes+1),
		string([]byte{0xff, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd'}),
	} {
		if err := ValidatePassword(password); !errors.Is(err, ErrInvalidPassword) {
			t.Errorf("ValidatePassword(%q) error=%v, want ErrInvalidPassword", password, err)
		}
	}
}

func TestPasswordHasherHashAndVerify(t *testing.T) {
	hasher, err := NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	password := "correct-horse-battery"
	encoded, err := hasher.HashPassword(ctx, password)
	if err != nil {
		t.Fatalf("HashPassword: %v", err)
	}
	if !strings.HasPrefix(encoded, "$argon2id$v=19$m=65536,t=3,p=1$") {
		t.Fatalf("hash encoding does not declare the required Argon2id parameters: %s", encoded)
	}
	ok, err := hasher.VerifyPassword(ctx, encoded, password)
	if err != nil || !ok {
		t.Fatalf("VerifyPassword(correct)=(%v,%v), want (true,nil)", ok, err)
	}
	ok, err = hasher.VerifyPassword(ctx, encoded, "incorrect-horse-battery")
	if err != nil || ok {
		t.Fatalf("VerifyPassword(incorrect)=(%v,%v), want (false,nil)", ok, err)
	}
}

func TestPasswordHasherRejectsUnsafeAndMalformedHashesBeforeKDF(t *testing.T) {
	hasher, err := NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name string
		hash string
		want error
	}{
		{name: "malformed", hash: "$argon2id$v=19$m=bad,t=3,p=1$c2FsdA$a2V5", want: ErrMalformedHash},
		{name: "unsupported version", hash: "$argon2id$v=16$m=65536,t=3,p=1$c2FsdA$a2V5", want: ErrUnsafeHash},
		{name: "excess memory", hash: "$argon2id$v=19$m=262145,t=3,p=1$MDEyMzQ1Njc4OWFiY2RlZg$MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY", want: ErrUnsafeHash},
		{name: "excess time", hash: "$argon2id$v=19$m=65536,t=7,p=1$MDEyMzQ1Njc4OWFiY2RlZg$MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY", want: ErrUnsafeHash},
		{name: "excess parallelism", hash: "$argon2id$v=19$m=65536,t=3,p=5$MDEyMzQ1Njc4OWFiY2RlZg$MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY", want: ErrUnsafeHash},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := hasher.VerifyPassword(context.Background(), tc.hash, "correct-horse-battery")
			if !errors.Is(err, tc.want) {
				t.Fatalf("VerifyPassword error=%v, want %v", err, tc.want)
			}
		})
	}
}

func TestPasswordHasherHashHonorsContextWhileWaiting(t *testing.T) {
	hasher, err := NewPasswordHasher(1)
	if err != nil {
		t.Fatal(err)
	}
	hasher.work <- struct{}{}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := hasher.HashPassword(ctx, "correct-horse-battery"); !errors.Is(err, context.Canceled) {
		t.Fatalf("HashPassword cancelled context error=%v, want context.Canceled", err)
	}
}
