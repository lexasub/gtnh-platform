package main

import (
	"encoding/binary"
	"net"
	"testing"
	"time"
)

// Regression: a registered service (e.g. chunkstore with no chunk.requests)
// may legitimately send nothing for > idleTimeout. The idle cleanup must NOT
// kill it — a dead service surfaces as TCP RST/EOF, not a timer.
func TestHealthControlEcho(t *testing.T) {
	left, right := net.Pipe()
	defer left.Close()
	defer right.Close()
	r := NewRouter()
	cl := newClient(left)
	defer cl.Close()
	r.RegisterService("service", nil, cl)
	payload := make([]byte, 8)
	binary.BigEndian.PutUint64(payload, 7)
	go r.handleHealthRequest(cl, payload)
	_ = right.SetReadDeadline(time.Now().Add(time.Second))
	mt, got, err := readFrame(right, make([]byte, 32))
	if err != nil || mt != MsgHealthResponse || binary.BigEndian.Uint64(got) != 7 {
		t.Fatalf("unexpected health echo: type=%d payload=%x err=%v", mt, got, err)
	}
}

func TestCleanupSkipsRegisteredService(t *testing.T) {
	r := NewRouter()
	conn1, conn2 := net.Pipe()
	defer conn1.Close()
	defer conn2.Close()

	cl := newClient(conn1)
	r.RegisterService("chunkstore", []string{"chunk.requests"}, cl)

	// Simulate > idleTimeout of silence.
	cl.lastSeen.Store(time.Now().Add(-2 * idleTimeout).UnixNano())

	r.cleanupOnce()

	select {
	case <-cl.done:
		t.Fatal("registered service connection was closed by idle cleanup")
	default:
	}
	cl.Close()
}

// The idle cleanup must still kill plain (non-service) clients that go silent.
func TestCleanupKillsIdleClient(t *testing.T) {
	r := NewRouter()
	conn1, conn2 := net.Pipe()
	defer conn1.Close()
	defer conn2.Close()

	cl := newClient(conn1)
	r.Subscribe("test", cl)
	cl.lastSeen.Store(time.Now().Add(-2 * idleTimeout).UnixNano())

	r.cleanupOnce()

	select {
	case <-cl.done:
	default:
		t.Fatal("idle client connection was not closed")
	}
}
