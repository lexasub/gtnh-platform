package integration

import (
	"fmt"
	"net"
	"os"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

var gw testutil.GatewayAddress

func TestMain(m *testing.M) {
	sm := &testutil.ServiceManager{}
	cleanup := startServices(sm)
	code := m.Run()
	cleanup()
	os.Exit(code)
}

func startServices(sm *testutil.ServiceManager) func() {
	gw = testutil.DefaultGateway()

	// Start MessageRouter
	if err := sm.StartService(testutil.ServiceConfig{
		Name:   "routerd",
		Binary: "routerd",
		Args:   []string{"--port", "4000"},
		ReadyCheck: func() bool {
			conn, err := net.DialTimeout("tcp", "127.0.0.1:4000", 100*time.Millisecond)
			if err != nil {
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: routerd not available: %v\n", err)
		return sm.Shutdown
	}

	// Start Gateway
	if err := sm.StartService(testutil.ServiceConfig{
		Name:   "gatewayd",
		Binary: "gatewayd",
		Args:   []string{"--router-port", "4000", "--port", "7777", "--bulk-port", "7778"},
		ReadyCheck: func() bool {
			conn, err := net.DialTimeout("tcp", "127.0.0.1:7777", 100*time.Millisecond)
			if err != nil {
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: gatewayd not available: %v\n", err)
		return sm.Shutdown
	}

	// Start ChunkStore
	if err := sm.StartService(testutil.ServiceConfig{
		Name:   "chunkd",
		Binary: "chunkd",
		Args:   []string{"/tmp/gtnh-test-chunkdb", "5001", "127.0.0.1", "4000"},
		ReadyCheck: func() bool {
			conn, err := net.DialTimeout("tcp", "127.0.0.1:5001", 100*time.Millisecond)
			if err != nil {
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: chunkd not available: %v\n", err)
		return sm.Shutdown
	}

	// Start SimulationCore
	if err := sm.StartService(testutil.ServiceConfig{
		Name:   "simcored",
		Binary: "simcored",
		Args: []string{
			"127.0.0.1", "4000",   // router host, port
			"127.0.0.1", "5001",   // chunkstore host, port
			testutil.DataRoot + "/recipes",
			testutil.DataRoot + "/registry/consumers.csv",
			testutil.DataRoot + "/registry/producers.csv",
		},
		ReadyCheck: func() bool {
			// SimCore doesn't have a direct TCP port; check router connection by
			// verifying gateway is still up (proxy for "system is ready")
			conn, err := net.DialTimeout("tcp", "127.0.0.1:7777", 100*time.Millisecond)
			if err != nil {
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: simcored not available: %v\n", err)
		return sm.Shutdown
	}

	// Start MetaDB (inventory persistence)
	metadbDir, _ := os.MkdirTemp("", "gtnh-test-metadb")
	if err := sm.StartService(testutil.ServiceConfig{
		Name:    "metadbd",
		Binary:  "metadbd",
		WorkDir: metadbDir,
		ReadyCheck: func() bool {
			conn, err := net.DialTimeout("tcp", "127.0.0.1:5006", 100*time.Millisecond)
			if err != nil {
				// MetaDB may not expose :5006 immediately; check router
				conn2, err2 := net.DialTimeout("tcp", "127.0.0.1:4000", 100*time.Millisecond)
				if err2 == nil {
					conn2.Close()
					return true // router port is up, metadbd may still be connecting
				}
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: metadbd not available: %v\n", err)
		return sm.Shutdown
	}

	// Let services settle and subscribe to topics before tests start
	time.Sleep(3 * time.Second)

	return func() {
		sm.Shutdown()
		os.RemoveAll(metadbDir)
	}
}
