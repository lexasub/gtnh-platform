package integration

import (
	"fmt"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

var gw testutil.GatewayAddress

// clusterStop tears the cluster down. It is safe to call from every exit path —
// a normal return, a panic, and a signal handler — because the function
// startServices returns is already idempotent. Only the first call does work.
var clusterStop = func() {}

// TestMain starts one cluster, runs the suite, and guarantees teardown.
//
// The previous shape was `code := m.Run(); cleanup(); os.Exit(code)`, which
// has no teardown on any path that matters:
//
//   - os.Exit skips defers, so a deferred cleanup would not have run either.
//   - `-test.timeout` panics from the runtime's alarm goroutine, not from
//     here, so nothing in this frame can intercept it.
//   - SIGINT/SIGTERM killed the process outright; there was no handler.
//
// The leak is not theoretical: an orphaned cluster holds :5001/:7777 for
// minutes, and the next ctest run then fails for a reason that has nothing to
// do with the code under test. The per-service supervisor (see
// testutil/supervisor.go) is what makes this hold even for SIGKILL, which no
// in-process handler can catch; the paths below cover everything short of it.
func TestMain(m *testing.M) {
	clusterStop = startServices()

	// A second signal must always be able to kill the process. A handler that
	// waits for teardown forever would turn Ctrl-C into an unkillable hang,
	// so the second one restores the default disposition and re-raises.
	signals := make(chan os.Signal, 2)
	signal.Notify(signals, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		first := true
		for sig := range signals {
			if !first {
				signal.Stop(signals)
				_ = syscall.Kill(os.Getpid(), sig.(syscall.Signal))
				return
			}
			first = false
			fmt.Fprintf(os.Stderr, "\n=== integration: %s received, tearing down the cluster ===\n", sig)
			clusterStop()
			// Exit on our own terms: 128+n, the status a shell reports for a
			// signal-killed process. Distinct from 0 and from 1, so an
			// interrupted run is never mistaken for a clean pass.
			os.Exit(exitInterrupted)
		}
	}()

	code := runWithCleanup(m)
	clusterStop()
	signal.Stop(signals)
	os.Exit(code)
}

// exitInterrupted is the status for a run cut short by a signal. It is
// distinct from 0 (clean) and from 1 (test failure), and matches the 128+n
// convention a shell would report for signal n.
const exitInterrupted = 128 + int(syscall.SIGINT)

// runWithCleanup runs the suite and tears the cluster down even when the run
// ends in a panic.
//
// This is the only place that can do so for `-test.timeout`: the runtime raises
// that panic from its own alarm goroutine, so a defer in TestMain's frame would
// never run, and a recover in the frame that called m.Run would not see it
// either — it unwinds through here on its way out. The panic is re-raised after
// the cleanup so the runtime still prints its own report.
func runWithCleanup(m *testing.M) (code int) {
	defer func() {
		if r := recover(); r != nil {
			clusterStop()
			panic(r)
		}
	}()
	return m.Run()
}

// clusterStartError is a required service that could not be started.
//
// It is a hard failure, not a skip. The harness used to print
// "SKIP: chunkd not available" and return a no-op cleanup, so the suite ran
// against a cluster with no ChunkStore and produced "read tcp ...:7777:
// i/o timeout" — output that reads like a server regression and is not one,
// and a run that can look green while testing nothing. See startServices.
type clusterStartError struct {
	Service string
	Cause   error
}

func (e *clusterStartError) Error() string {
	return fmt.Sprintf("required service %s could not be started: %v", e.Service, e.Cause)
}

func (e *clusterStartError) Unwrap() error { return e.Cause }

// startServices starts the cluster and returns a teardown function that is
// safe to call any number of times, from any of the exit paths.
//
// Every failure here is fatal and reported as such. The caller sees a FAIL
// attributed to the harness, never a silent skip and never a suite that
// cheerfully runs on a broken world.
func startServices() func() {
	gw = testutil.DefaultGateway()
	projectRoot := filepath.Clean(filepath.Join(testutil.BuildRoot, ".."))
	registryRoot := filepath.Join(testutil.DataRoot, "registry")
	machinesYAML := filepath.Join(registryRoot, "machines.yaml")

	sm := &testutil.ServiceManager{}
	teardown := sync.OnceFunc(func() { sm.Shutdown() })

	// Refuse to start if a port is already taken. The port-conflict case is
	// the one that used to produce the false SKIP, and diagnosing it up
	// front beats discovering it as a pile of unrelated timeouts.
	if err := requirePortsFree(requiredClusterPorts); err != nil {
		teardown()
		failHarness("refusing to start: %v", err)
	}

	// Every temp dir is registered as soon as it exists so that a failure in
	// between cannot leave a database directory behind on disk.
	var dirs []string
	removeDirs := func() {
		for _, dir := range dirs {
			_ = os.RemoveAll(dir)
		}
	}
	newTempDir := func(pattern string) (string, error) {
		dir, err := os.MkdirTemp("", pattern)
		if err != nil {
			return "", err
		}
		dirs = append(dirs, dir)
		return dir, nil
	}
	cleanup := func() { teardown(); removeDirs() }

	chunkdbDir, err := newTempDir("gtnh-test-chunkdb-")
	if err != nil {
		cleanup()
		failHarness("cannot create isolated ChunkStore database: %v", err)
	}
	entityDir, err := newTempDir("gtnh-test-entitystate-")
	if err != nil {
		cleanup()
		failHarness("cannot create isolated EntityState database: %v", err)
	}
	dbPath := filepath.Join(entityDir, "db")
	if err := os.MkdirAll(dbPath, 0755); err != nil {
		cleanup()
		failHarness("cannot create EntityState db dir: %v", err)
	}
	metadbDir, err := newTempDir("gtnh-test-metadb-")
	if err != nil {
		cleanup()
		failHarness("cannot create isolated MetaDB directory: %v", err)
	}

	if err := os.Chdir(projectRoot); err != nil {
		cleanup()
		failHarness("cannot enter project root: %v", err)
	}

	// start is the single place a service is launched. Any failure is fatal:
	// there is no configuration of this suite in which a missing service is
	// an acceptable outcome.
	start := func(cfg testutil.ServiceConfig, ready func(*testutil.ManagedService) bool) *testutil.ManagedService {
		cfg.ReadyCheck = ready
		service, err := sm.StartService(cfg)
		if err != nil {
			cleanup()
			failHarness("%v", &clusterStartError{Service: cfg.Name, Cause: err})
		}
		return service
	}

	router := start(testutil.ServiceConfig{
		Name:   "routerd",
		Binary: "routerd",
		Args:   []string{"--port", "4000"},
	}, func(*testutil.ManagedService) bool {
		conn, err := net.DialTimeout("tcp", "127.0.0.1:4000", 100*time.Millisecond)
		if err != nil {
			return false
		}
		conn.Close()
		return true
	})

	start(testutil.ServiceConfig{
		Name:   "pipe_networkd",
		Binary: "pipe_networkd",
		Args:   []string{"--router-host", "127.0.0.1", "--router-port", "4000"},
	}, func(*testutil.ManagedService) bool {
		return testutil.RouterServiceHasRegistration(router.Output(), "pipe_network")
	})

	start(testutil.ServiceConfig{
		Name:   "chunkd",
		Binary: "chunkd",
		Args:   []string{chunkdbDir, "5001", "127.0.0.1", "4000"},
	}, func(*testutil.ManagedService) bool {
		return testutil.RouterServiceHasSubscriptions(router.Output(), "chunkstore", "chunk.requests")
	})

	start(testutil.ServiceConfig{
		Name:   "entitystated",
		Binary: "entitystated",
		Args:   []string{dbPath},
	}, func(*testutil.ManagedService) bool {
		return testutil.RouterServiceHasRegistration(router.Output(), "entitystated")
	})

	start(testutil.ServiceConfig{
		Name:   "simcored",
		Binary: "simcored",
		Args: []string{
			"127.0.0.1", "4000",
			"127.0.0.1", "5001",
			filepath.Join(testutil.DataRoot, "recipes"),
			machinesYAML,
		},
	}, func(*testutil.ManagedService) bool {
		return testutil.RouterServiceHasSubscription(router.Output(), "simcore", "quest.book.open")
	})

	start(testutil.ServiceConfig{
		Name:   "gatewayd",
		Binary: "gatewayd",
		Args:   []string{"--router-port", "4000", "--port", "7777", "--bulk-port", "7778"},
	}, func(*testutil.ManagedService) bool {
		return testutil.RouterServiceHasSubscriptions(router.Output(), "gateway",
			"metadb.player.online", "quest.progress.updated", "quest.completed.notification")
	})

	start(testutil.ServiceConfig{
		Name:    "metadbd",
		Binary:  "metadbd",
		WorkDir: metadbDir,
	}, func(*testutil.ManagedService) bool {
		return testutil.RouterServiceHasRegistration(router.Output(), "metadb")
	})

	return cleanup
}

// requiredClusterPorts are the fixed ports the cluster binds. A leftover
// cluster from an earlier run owns them, which is the exact condition that
// used to turn into a silent skip.
var requiredClusterPorts = []int{4000, 5001, 7777, 7778}

func requirePortsFree(ports []int) error {
	var busy []string
	for _, port := range ports {
		conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 100*time.Millisecond)
		if err != nil {
			continue // nothing listening
		}
		conn.Close()
		busy = append(busy, fmt.Sprintf(":%d", port))
	}
	if len(busy) == 0 {
		return nil
	}
	return fmt.Errorf("port(s) %s already in use — another service cluster is still running, "+
		"most likely leaked by an interrupted earlier run; "+
		"find it with: pgrep -af 'chunkd|gatewayd|simcored_exec|entitystated|metadbd|routerd'",
		strings.Join(busy, ", "))
}

// failHarness reports a harness failure in the one form `go test` counts as a
// failure, and exits.
//
// It deliberately does not use t.Skip: a skip is reported as a pass in the
// exit code and in most CI summaries, which is precisely the failure mode
// this replaces. os.Exit(1) makes ctest and `go test` both see a failure, and
// the message on stderr cannot be mistaken for test output.
func failHarness(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "\n=== integration: FATAL: "+format+" ===\n", args...)
	fmt.Fprintf(os.Stderr, "=== integration: refusing to run the suite against an incomplete cluster ===\n")
	os.Exit(1)
}
