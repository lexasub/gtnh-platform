package main

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"path/filepath"
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"github.com/gtnh-platform/protocol/generated/go/Protocol"
	"github.com/gtnh/platform/gtnh-common/auth"
)

func buildAccountAuthRequest(action Protocol.AccountAuthAction, username, password string) []byte {
	builder := flatbuffers.NewBuilder(128)
	usernameOffset := builder.CreateString(username)
	passwordOffset := builder.CreateString(password)
	Protocol.AccountAuthRequestStart(builder)
	Protocol.AccountAuthRequestAddAction(builder, action)
	Protocol.AccountAuthRequestAddUsername(builder, usernameOffset)
	Protocol.AccountAuthRequestAddPassword(builder, passwordOffset)
	offset := Protocol.AccountAuthRequestEnd(builder)
	Protocol.FinishAccountAuthRequestBuffer(builder, offset)
	return append([]byte(nil), builder.FinishedBytes()...)
}

func TestAccountAuthRPCRegisterAndLoginReturnStablePlayer(t *testing.T) {
	m, err := NewMetaDB(filepath.Join(t.TempDir(), "auth.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer m.db.Close()
	hasher, err := auth.NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	request := buildAccountAuthRequest(Protocol.AccountAuthActionREGISTER, "New_User", "correct-horse-battery")
	requestRoot, err := verifyRoot(request, fbAccountAuthRequest)
	if err != nil {
		t.Fatalf("verify auth request: %v", err)
	}
	parsed := &Protocol.AccountAuthRequest{}
	parsed.Init(request, requestRoot)
	if parsed.Action() != Protocol.AccountAuthActionREGISTER || string(parsed.Username()) != "New_User" || string(parsed.Password()) != "correct-horse-battery" {
		t.Fatalf("parsed request action=%v username=%q password bytes=%d", parsed.Action(), parsed.Username(), len(parsed.Password()))
	}
	registered := Protocol.GetRootAsAccountAuthResponse(dispatchAccountAuth(context.Background(), request, m, hasher), 0)
	if registered.Status() != Protocol.AccountAuthStatusSUCCESS || registered.PlayerId() == 0 || len(registered.AccountId()) == 0 {
		t.Fatalf("registration response status=%v player=%d account=%q", registered.Status(), registered.PlayerId(), registered.AccountId())
	}

	login := buildAccountAuthRequest(Protocol.AccountAuthActionLOGIN, "new_user", "correct-horse-battery")
	loggedIn := Protocol.GetRootAsAccountAuthResponse(dispatchAccountAuth(context.Background(), login, m, hasher), 0)
	if loggedIn.Status() != Protocol.AccountAuthStatusSUCCESS || loggedIn.PlayerId() != registered.PlayerId() || string(loggedIn.AccountId()) != string(registered.AccountId()) {
		t.Fatalf("login did not resolve stable identity: registered=%q/%d login=%q/%d", registered.AccountId(), registered.PlayerId(), loggedIn.AccountId(), loggedIn.PlayerId())
	}
}

func TestAccountAuthRPCDoesNotEnumerateLoginFailures(t *testing.T) {
	m, err := NewMetaDB(filepath.Join(t.TempDir(), "auth.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer m.db.Close()
	hasher, err := auth.NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := m.RegisterAccount(context.Background(), "known_user", testPasswordHash(t)); err != nil {
		t.Fatal(err)
	}
	wrong := buildAccountAuthRequest(Protocol.AccountAuthActionLOGIN, "known_user", "wrong-password")
	unknown := buildAccountAuthRequest(Protocol.AccountAuthActionLOGIN, "missing_user", "wrong-password")
	wrongResult := Protocol.GetRootAsAccountAuthResponse(dispatchAccountAuth(context.Background(), wrong, m, hasher), 0)
	unknownResult := Protocol.GetRootAsAccountAuthResponse(dispatchAccountAuth(context.Background(), unknown, m, hasher), 0)
	if wrongResult.Status() != Protocol.AccountAuthStatusINVALID_CREDENTIALS || unknownResult.Status() != wrongResult.Status() {
		t.Fatalf("failure status differs: wrong=%v unknown=%v", wrongResult.Status(), unknownResult.Status())
	}
	if wrongResult.PlayerId() != 0 || unknownResult.PlayerId() != 0 || len(wrongResult.AccountId()) != 0 || len(unknownResult.AccountId()) != 0 {
		t.Fatal("failed login leaked account identity")
	}
}

func TestAccountAuthRPCRejectsMalformedAndOversizedInputs(t *testing.T) {
	m, err := NewMetaDB(filepath.Join(t.TempDir(), "auth.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer m.db.Close()
	hasher, err := auth.NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	for _, frame := range [][]byte{nil, {1, 2, 3, 4}} {
		result := Protocol.GetRootAsAccountAuthResponse(dispatchAccountAuth(context.Background(), frame, m, hasher), 0)
		if result.Status() != Protocol.AccountAuthStatusINTERNAL_ERROR {
			t.Fatalf("malformed input status=%v", result.Status())
		}
	}
	longPassword := make([]byte, auth.MaxPasswordBytes+1)
	for i := range longPassword {
		longPassword[i] = 'x'
	}
	frame := buildAccountAuthRequest(Protocol.AccountAuthActionREGISTER, "large_pass", string(longPassword))
	result := Protocol.GetRootAsAccountAuthResponse(dispatchAccountAuth(context.Background(), frame, m, hasher), 0)
	if result.Status() != Protocol.AccountAuthStatusINVALID_CREDENTIALS {
		t.Fatalf("oversized password status=%v", result.Status())
	}
}

func TestAccountAuthRPCUnknownUserUsesExpensiveHashPath(t *testing.T) {
	m := newAccountTestMetaDB(t)
	hasher, err := auth.NewPasswordHasher(1)
	if err != nil {
		t.Fatal(err)
	}
	request := buildAccountAuthRequest(Protocol.AccountAuthActionLOGIN,
		"missing_user", "incorrect-horse-battery")
	response, err := accountAuthRoundTripTest(t, m, hasher, request)
	if err != nil {
		t.Fatalf("account auth round trip: %v", err)
	}
	result := Protocol.GetRootAsAccountAuthResponse(response, 0)
	if result.Status() != Protocol.AccountAuthStatusINVALID_CREDENTIALS {
		t.Fatalf("unknown user status=%v, want invalid credentials", result.Status())
	}
}

func accountAuthRoundTripTest(t *testing.T, m *MetaDB, hasher *auth.PasswordHasher, frame []byte) ([]byte, error) {
	t.Helper()
	client, server := net.Pipe()
	done := make(chan struct{})
	go func() {
		handleAccountAuthConnection(server, m, hasher)
		close(done)
	}()
	defer client.Close()
	var prefix [4]byte
	binary.BigEndian.PutUint32(prefix[:], uint32(len(frame)))
	if _, err := client.Write(prefix[:]); err != nil {
		return nil, err
	}
	if _, err := client.Write(frame); err != nil {
		return nil, err
	}
	if _, err := io.ReadFull(client, prefix[:]); err != nil {
		return nil, err
	}
	n := binary.BigEndian.Uint32(prefix[:])
	if n == 0 || n > accountAuthFrameLimit {
		return nil, errors.New("invalid auth response length")
	}
	response := make([]byte, n)
	if _, err := io.ReadFull(client, response); err != nil {
		return nil, err
	}
	client.Close()
	<-done
	return response, nil
}

func TestAccountAuthConnectionRejectsOversizedFrame(t *testing.T) {
	m := newAccountTestMetaDB(t)
	hasher, err := auth.NewPasswordHasher(2)
	if err != nil {
		t.Fatal(err)
	}
	client, server := net.Pipe()
	done := make(chan struct{})
	go func() {
		handleAccountAuthConnection(server, m, hasher)
		close(done)
	}()
	var header [4]byte
	binary.BigEndian.PutUint32(header[:], accountAuthFrameLimit+1)
	if _, err := client.Write(header[:]); err != nil {
		t.Fatalf("write oversized frame length: %v", err)
	}
	_ = client.SetReadDeadline(time.Now().Add(time.Second))
	var response [1]byte
	if _, err := client.Read(response[:]); err != io.EOF {
		t.Fatalf("oversized request did not close the connection: err=%v", err)
	}
	_ = client.Close()
	<-done
}

func TestLegacyNumericJSONCannotReadOrMutateProfiles(t *testing.T) {
	m, err := NewMetaDB(filepath.Join(t.TempDir(), "legacy.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer m.db.Close()
	if err := m.UpdateInventorySlot(42, 0, 123, 1); err != nil {
		t.Fatal(err)
	}
	if err := m.SavePlayerPosition(42, 1, 2, 3); err != nil {
		t.Fatal(err)
	}
	actions := []Request{
		{Action: "login", PlayerID: 42},
		{Action: "logout", PlayerID: 42, Data: map[string]interface{}{
			"slots": []interface{}{map[string]interface{}{
				"slot": float64(0), "block_id": float64(999), "count": float64(2),
			}}, "x": float64(9), "y": float64(9), "z": float64(9),
		}},
		{Action: "update_slot", PlayerID: 42, Data: map[string]interface{}{
			"slot": float64(0), "block_id": float64(999), "count": float64(2),
		}},
		{Action: "delete_slot", PlayerID: 42, Data: map[string]interface{}{"slot": float64(0)}},
		{Action: "save_position", PlayerID: 42, Data: map[string]interface{}{
			"x": float64(9), "y": float64(9), "z": float64(9),
		}},
	}
	for _, request := range actions {
		response := handleRequest(m, request)
		if response.Success || response.Data != nil {
			t.Errorf("legacy action %q unexpectedly accessed profile data: %+v", request.Action, response)
		}
	}
	found, blockID, count, err := m.GetInventorySlot(42, 0)
	if err != nil || !found || blockID != 123 || count != 1 {
		t.Fatalf("legacy action mutated inventory found=%t item=%d count=%d err=%v", found, blockID, count, err)
	}
	position, err := m.GetPlayerPosition(42)
	if err != nil || position.X != 1 || position.Y != 2 || position.Z != 3 {
		t.Fatalf("legacy action mutated position=%+v err=%v", position, err)
	}
}
