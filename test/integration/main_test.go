package integration

import (
	"fmt"
	"net"
	"os"
	"path/filepath"
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
	projectRoot := filepath.Clean(filepath.Join(testutil.DataRoot, ".."))
	registryRoot := filepath.Join(testutil.DataRoot, "registry")
	machinesYAML := filepath.Join(registryRoot, "machines.yaml")
	chunkdbDir, err := os.MkdirTemp("", "gtnh-test-chunkdb-")
	if err != nil {
		fmt.Printf("SKIP: cannot create isolated ChunkStore database: %v\n", err)
		return sm.Shutdown
	}
	var metadbDir string
	cleanup := func() {
		sm.Shutdown()
		os.RemoveAll(chunkdbDir)
		if metadbDir != "" {
			os.RemoveAll(metadbDir)
		}
	}
	if err := os.Chdir(projectRoot); err != nil {
		fmt.Printf("SKIP: cannot enter project root: %v\n", err)
		cleanup()
		return func() {}
	}

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
		cleanup()
		return func() {}
	}

	if err := sm.StartService(testutil.ServiceConfig{
		Name:       "pipe_networkd",
		Binary:     "pipe_networkd",
		Args:       []string{"--router-host", "127.0.0.1", "--router-port", "4000"},
		ReadyCheck: func() bool { return true },
	}); err != nil {
		fmt.Printf("SKIP: pipenetworkd not available: %v\n", err)
		cleanup()
		return func() {}
	}

	if err := sm.StartService(testutil.ServiceConfig{
		Name:   "chunkd",
		Binary: "chunkd",
		Args:   []string{chunkdbDir, "5001", "127.0.0.1", "4000"},
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
		cleanup()
		return func() {}
	}

	if err := sm.StartService(testutil.ServiceConfig{
		Name:   "entitystated",
		Binary: "entitystated",
		ReadyCheck: func() bool {
			conn, err := net.DialTimeout("tcp", "127.0.0.1:5200", 100*time.Millisecond)
			if err != nil {
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: entitystated not available: %v\n", err)
		cleanup()
		return func() {}
	}

	if err := sm.StartService(testutil.ServiceConfig{
		Name:   "simcored",
		Binary: "simcored",
		Args: []string{
			"127.0.0.1", "4000",
			"127.0.0.1", "5001",
			filepath.Join(testutil.DataRoot, "recipes"),
			machinesYAML,
		},
		ReadyCheck: func() bool {
			conn, err := net.DialTimeout("tcp", "127.0.0.1:4000", 100*time.Millisecond)
			if err != nil {
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: simcored not available: %v\n", err)
		cleanup()
		return func() {}
	}

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
		cleanup()
		return func() {}
	}

	metadbDir, err = os.MkdirTemp("", "gtnh-test-metadb-")
	if err != nil {
		fmt.Printf("SKIP: cannot create isolated MetaDB directory: %v\n", err)
		cleanup()
		return func() {}
	}
	if err := sm.StartService(testutil.ServiceConfig{
		Name:    "metadbd",
		Binary:  "metadbd",
		WorkDir: metadbDir,
		ReadyCheck: func() bool {
			conn, err := net.DialTimeout("tcp", "127.0.0.1:5006", 100*time.Millisecond)
			if err != nil {
				return false
			}
			conn.Close()
			return true
		},
	}); err != nil {
		fmt.Printf("SKIP: metadbd not available: %v\n", err)
		cleanup()
		return func() {}
	}

	time.Sleep(3 * time.Second)
	return cleanup
}
