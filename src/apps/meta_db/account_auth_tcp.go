package main

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"github.com/gtnh-platform/protocol/generated/go/Protocol"
	"github.com/gtnh/platform/gtnh-common/auth"
)

const (
	defaultAccountAuthBind = "127.0.0.1:5007"
	accountAuthFrameLimit  = 64 << 10
	accountAuthRPCTimeout  = 3 * time.Second
)

func configuredPrivateBind(envKey, fallback string) (string, error) {
	address := os.Getenv(envKey)
	if address == "" {
		return fallback, nil
	}
	host, _, err := net.SplitHostPort(address)
	if err != nil {
		return "", fmt.Errorf("%s must be an IP:port: %w", envKey, err)
	}
	ip := net.ParseIP(host)
	if ip == nil || ip.IsUnspecified() || (!ip.IsLoopback() && !ip.IsPrivate()) {
		return "", fmt.Errorf("%s must be loopback or a private interface address", envKey)
	}
	return address, nil
}

func accountAuthBind() (string, error) {
	return configuredPrivateBind("GTNH_METADB_AUTH_BIND", defaultAccountAuthBind)
}

func startAccountAuthListener(m *MetaDB, hasher *auth.PasswordHasher) error {
	address, err := accountAuthBind()
	if err != nil {
		return err
	}
	listener, err := net.Listen("tcp", address)
	if err != nil {
		return fmt.Errorf("listen on MetaDB account-auth address %s: %w", address, err)
	}
	log.Printf("MetaDB account-auth listener on %s", address)
	go func() {
		defer listener.Close()
		connections := make(chan struct{}, 64)
		for {
			conn, err := listener.Accept()
			if err != nil {
				log.Printf("MetaDB account-auth accept error: %v", err)
				return
			}
			select {
			case connections <- struct{}{}:
				go func(conn net.Conn) {
					defer func() { <-connections }()
					handleAccountAuthConnection(conn, m, hasher)
				}(conn)
			default:
				_ = conn.Close()
			}
		}
	}()
	return nil
}

func handleAccountAuthConnection(conn net.Conn, m *MetaDB, hasher *auth.PasswordHasher) {
	defer conn.Close()
	defer func() {
		if recover() != nil {
			log.Printf("MetaDB account-auth: malformed request caused a parser panic; connection closed")
		}
	}()
	deadline := time.Now().Add(accountAuthRPCTimeout)
	_ = conn.SetDeadline(deadline)

	var size [4]byte
	if _, err := io.ReadFull(conn, size[:]); err != nil {
		return
	}
	frameLen := binary.BigEndian.Uint32(size[:])
	if frameLen == 0 || frameLen > accountAuthFrameLimit {
		return
	}
	frame := make([]byte, int(frameLen))
	if _, err := io.ReadFull(conn, frame); err != nil {
		return
	}

	if time.Until(deadline) <= 0 {
		return
	}
	ctx, cancel := context.WithDeadline(context.Background(), deadline)
	defer cancel()
	response := dispatchAccountAuth(ctx, frame, m, hasher)
	if len(response) == 0 || len(response) > accountAuthFrameLimit || time.Now().After(deadline) {
		return
	}
	binary.BigEndian.PutUint32(size[:], uint32(len(response)))
	if writeAll(conn, size[:]) != nil || writeAll(conn, response) != nil {
		return
	}
}

func writeAll(w io.Writer, data []byte) error {
	for len(data) > 0 {
		n, err := w.Write(data)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		data = data[n:]
	}
	return nil
}

func dispatchAccountAuth(ctx context.Context, frame []byte, m *MetaDB, hasher *auth.PasswordHasher) (response []byte) {
	defer func() {
		if recover() != nil {
			response = buildAccountAuthResponse(Protocol.AccountAuthStatusINTERNAL_ERROR, "", 0)
		}
	}()

	root, err := verifyRoot(frame, fbAccountAuthRequest)
	if err != nil {
		return buildAccountAuthResponse(Protocol.AccountAuthStatusINTERNAL_ERROR, "", 0)
	}
	request := &Protocol.AccountAuthRequest{}
	request.Init(frame, root)
	username := string(request.Username())
	password := string(request.Password())
	if len(username) > auth.MaxUsernameBytes+2 || len(password) > auth.MaxPasswordBytes {
		return buildAccountAuthResponse(Protocol.AccountAuthStatusINVALID_CREDENTIALS, "", 0)
	}
	canonical, err := auth.CanonicalUsername(username)
	if err != nil || auth.ValidatePassword(password) != nil {
		return buildAccountAuthResponse(Protocol.AccountAuthStatusINVALID_CREDENTIALS, "", 0)
	}

	var account Account
	switch request.Action() {
	case Protocol.AccountAuthActionREGISTER:
		passwordHash, err := hasher.HashPassword(ctx, password)
		if err != nil {
			return buildAccountAuthResponse(Protocol.AccountAuthStatusRATE_LIMITED, "", 0)
		}
		account, err = m.RegisterAccount(ctx, canonical, passwordHash)
		if errors.Is(err, ErrAccountExists) {
			return buildAccountAuthResponse(Protocol.AccountAuthStatusUSERNAME_TAKEN, "", 0)
		}
		if err != nil {
			return buildAccountAuthResponse(Protocol.AccountAuthStatusINTERNAL_ERROR, "", 0)
		}
	case Protocol.AccountAuthActionLOGIN:
		account, err = m.AuthenticateAccount(ctx, canonical, password, hasher)
		if errors.Is(err, ErrInvalidCredential) {
			return buildAccountAuthResponse(Protocol.AccountAuthStatusINVALID_CREDENTIALS, "", 0)
		}
		if err != nil {
			return buildAccountAuthResponse(Protocol.AccountAuthStatusINTERNAL_ERROR, "", 0)
		}
	default:
		return buildAccountAuthResponse(Protocol.AccountAuthStatusINVALID_CREDENTIALS, "", 0)
	}
	return buildAccountAuthResponse(Protocol.AccountAuthStatusSUCCESS, account.ID, uint64(account.PlayerID))
}

func buildAccountAuthResponse(status Protocol.AccountAuthStatus, accountID string, playerID uint64) []byte {
	builder := flatbuffers.NewBuilder(128)
	accountIDOffset := flatbuffers.UOffsetT(0)
	if accountID != "" {
		accountIDOffset = builder.CreateString(accountID)
	}
	Protocol.AccountAuthResponseStart(builder)
	Protocol.AccountAuthResponseAddStatus(builder, status)
	Protocol.AccountAuthResponseAddAccountId(builder, accountIDOffset)
	Protocol.AccountAuthResponseAddPlayerId(builder, playerID)
	offset := Protocol.AccountAuthResponseEnd(builder)
	Protocol.FinishAccountAuthResponseBuffer(builder, offset)
	return append([]byte(nil), builder.FinishedBytes()...)
}
