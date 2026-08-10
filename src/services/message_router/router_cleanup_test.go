package main

import (
	"net"
	"testing"
	"time"
)

// Regression: a registered service (e.g. chunkstore with no chunk.requests)
// may legitimately send nothing for > idleTimeout. The idle cleanup must NOT
// kill it — a dead service surfaces as TCP RST/EOF, not a timer.
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

// The idle cleanup must still kill plain (non-service) clients that go silent —
// otherwise zombies accumulate in the subscriber map.
func TestCleanupStillKillsIdlePlainClient(t *testing.T) {
	r := NewRouter()
	conn1, conn2 := net.Pipe()
	defer conn1.Close()
	defer conn2.Close()

	cl := newClient(conn1)
	r.Subscribe("world.blocks.changed", cl)
	cl.lastSeen.Store(time.Now().Add(-2 * idleTimeout).UnixNano())

	r.cleanupOnce()

	select {
	case <-cl.done:
		// expected — plain client got idle-killed
	default:
		t.Fatal("idle plain client was NOT killed by cleanup")
	}
}
