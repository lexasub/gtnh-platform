package testutil

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// fakeService installs a shell script under a private BuildRoot so the harness
// never touches a real daemon.
func fakeService(t *testing.T, name, script string) {
	t.Helper()
	oldBuildRoot := BuildRoot
	t.Cleanup(func() { BuildRoot = oldBuildRoot })

	buildRoot := t.TempDir()
	binDir := filepath.Join(buildRoot, "bin")
	if err := os.MkdirAll(binDir, 0o755); err != nil {
		t.Fatalf("create bin directory: %v", err)
	}
	if err := os.WriteFile(filepath.Join(binDir, name), []byte(script), 0o755); err != nil {
		t.Fatalf("write fake service: %v", err)
	}
	BuildRoot = buildRoot
}

// TestSupervisedServiceDiesWithTheTestBinary is the contract that makes the
// harness leak-free: a service must not outlive the process that started it,
// even when that process is SIGKILLed and gets no chance to run anything.
//
// The process that owns the cluster has to be a separate process, because this
// test binary cannot SIGKILL itself and keep reporting results. So the child
// below is a second copy of this same binary that starts a service and then
// waits to be killed. The real supervisor, the real guard pipe and the real
// SIGKILL are all in play; only the reporting moves to the parent.
//
// This is the case that used to leak: a plain `defer cleanup()` and a
// recover() both pass this test, because neither of them runs under SIGKILL.
func TestSupervisedServiceDiesWithTheTestBinary(t *testing.T) {
	dir := t.TempDir()
	pidFile := filepath.Join(dir, "service.pid")

	self, err := os.Executable()
	if err != nil {
		t.Fatalf("resolve test binary: %v", err)
	}
	child := exec.Command(self, "-test.run=TestHelperClusterOwner", "-test.timeout=120s")
	child.Env = append(os.Environ(),
		childOwnerEnv+"=1",
		childPidFileEnv+"="+pidFile,
	)
	// Give the owner (and through it the supervisor) a real file for output,
	// so anything it prints survives the SIGKILL that closes a pipe.
	ownerLog, logErr := os.Create(filepath.Join(dir, "owner.log"))
	if logErr != nil {
		t.Fatalf("create owner log: %v", logErr)
	}
	child.Stdout = ownerLog
	child.Stderr = ownerLog
	if err := child.Start(); err != nil {
		t.Fatalf("start cluster owner: %v", err)
	}
	// Reap the child whatever happens, so this test never leaves a stray.
	t.Cleanup(func() {
		_ = syscall.Kill(child.Process.Pid, syscall.SIGKILL)
		_ = child.Wait()
	})

	// Wait for the child's service to come up.
	var pid int
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		if raw, err := os.ReadFile(pidFile); err == nil {
			if pid, err = strconv.Atoi(strings.TrimSpace(string(raw))); err == nil {
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	if pid == 0 {
		t.Fatalf("the cluster owner's service never reported a pid (file %s)", pidFile)
	}
	if !processAlive(pid) {
		t.Fatalf("service pid %d is not running before the owner is killed", pid)
	}

	// SIGKILL the owner: no handler, no defer, no cleanup can run in it.
	if err := syscall.Kill(child.Process.Pid, syscall.SIGKILL); err != nil {
		t.Fatalf("SIGKILL the cluster owner: %v", err)
	}
	_ = child.Wait()

	// The supervisor must notice the closed guard pipe and take the service
	// with it. Allow a moment: this is a kernel-scheduled reaction, not a
	// signal delivery.
	gone := false
	deadline = time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		if !processAlive(pid) {
			gone = true
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !gone {
		_ = syscall.Kill(pid, syscall.SIGKILL)
		t.Fatalf("service pid %d outlived the SIGKILLed process that started it", pid)
	}
}

const (
	childOwnerEnv   = "GTNH_TEST_CHELPER_OWNER"
	childPidFileEnv = "GTNH_TEST_CHELPER_PIDFILE"
)

// TestHelperClusterOwner is not a real test. It is the child process used by
// TestSupervisedServiceDiesWithTheTestBinary: it starts one supervised service
// and then blocks until it is SIGKILLed, so the parent's SIGKILL lands while the
// cluster is live.
func TestHelperClusterOwner(t *testing.T) {
	if os.Getenv(childOwnerEnv) != "1" {
		t.Skip("helper process for TestSupervisedServiceDiesWithTheTestBinary")
	}
	pidFile := os.Getenv(childPidFileEnv)
	// The fake service must be a single foreground process, like a real
	// service binary. A script that backgrounds a child (e.g. `sleep 300 &`)
	// would exit immediately and orphan that child, which is a property of the
	// script and not of the supervisor, and would make this test prove nothing.
	fakeService(t, "leakyd", "#!/bin/sh\necho $$ > "+pidFile+"\nexec sleep 300\n")

	sm := &ServiceManager{}
	if _, err := sm.StartService(ServiceConfig{
		Name:   "leakyd",
		Binary: "leakyd",
		ReadyCheck: func(*ManagedService) bool {
			raw, err := os.ReadFile(pidFile)
			return err == nil && strings.TrimSpace(string(raw)) != ""
		},
	}); err != nil {
		t.Fatalf("start fake service: %v", err)
	}
	// Deliberately no Shutdown: block here until the parent SIGKILLs us, which
	// is the whole point. Returning would close the guard pipe by exiting
	// normally and prove nothing.
	select {}
}

// TestServiceStaysUpWhileTheParentLives is the other half of the contract: a
// watchdog that fires on the guard pipe too eagerly would kill every service at
// startup and make the whole suite meaningless. Several services share one
// guard pipe, so a later start must not be told the parent is already gone.
func TestServiceStaysUpWhileTheParentLives(t *testing.T) {
	fakeService(t, "steadyfd", "#!/bin/sh\nexec sleep 300\n")

	sm := &ServiceManager{}
	t.Cleanup(sm.Shutdown)
	service, err := sm.StartService(ServiceConfig{
		Name:       "steadyfd",
		Binary:     "steadyfd",
		ReadyCheck: func(*ManagedService) bool { return true },
	})
	if err != nil {
		t.Fatalf("start fake service: %v", err)
	}
	if _, err := sm.StartService(ServiceConfig{
		Name:       "steadyfd2",
		Binary:     "steadyfd",
		ReadyCheck: func(*ManagedService) bool { return true },
	}); err != nil {
		t.Fatalf("start second fake service: %v", err)
	}

	// Longer than the supervisor's post-exit grace, so a watchdog that fires
	// eagerly is caught rather than passing by luck.
	time.Sleep(2 * time.Second)
	for i, svc := range sm.services {
		if svc.Exited() {
			_, _, serviceErr := svc.snapshot()
			t.Fatalf("service %d exited while the parent was alive (serviceErr=%v)", i, serviceErr)
		}
	}
	if pid := service.ServicePID(); pid == 0 || !processAlive(pid) {
		t.Fatalf("service pid %d is not running", service.ServicePID())
	}
}

// TestServiceExitIsReportedEvenThoughItBecomesAZombie pins the reason Exited
// cannot rely on kill(pid, 0): the service is a child of its supervisor, so
// between the service dying and the supervisor reaping it, the pid still exists
// as a zombie and answers signal 0 as a live process.
func TestServiceExitIsReportedEvenThoughItBecomesAZombie(t *testing.T) {
	fakeService(t, "quitterd", "#!/bin/sh\nexit 4\n")

	sm := &ServiceManager{}
	t.Cleanup(sm.Shutdown)
	service, err := sm.StartService(ServiceConfig{
		Name:       "quitterd",
		Binary:     "quitterd",
		ReadyCheck: func(*ManagedService) bool { return true },
	})
	if err == nil {
		t.Fatal("StartService accepted a service that exited immediately")
	}
	if !strings.Contains(err.Error(), "exited before readiness") {
		t.Fatalf("StartService error = %q, want an exit-before-readiness error", err)
	}
	if !service.Exited() {
		_, _, serviceErr := service.snapshot()
		t.Fatalf("Exited() = false for a service that exited (serviceErr=%v)", serviceErr)
	}
}

// TestShutdownIsIdempotent covers the exit paths: the signal handler and the
// deferred teardown can both run, in either order, and a test may also call
// Shutdown directly.
func TestShutdownIsIdempotent(t *testing.T) {
	fakeService(t, "idempotentd", "#!/bin/sh\nexec sleep 300\n")

	sm := &ServiceManager{}
	service, err := sm.StartService(ServiceConfig{
		Name:       "idempotentd",
		Binary:     "idempotentd",
		ReadyCheck: func(*ManagedService) bool { return true },
	})
	if err != nil {
		t.Fatalf("start fake service: %v", err)
	}

	done := make(chan struct{})
	go func() {
		defer close(done)
		sm.Shutdown()
		sm.Shutdown()
		sm.Shutdown()
	}()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("repeated Shutdown did not return within 10s")
	}
	if pid := service.ServicePID(); pid != 0 && processAlive(pid) {
		_ = syscall.Kill(pid, syscall.SIGKILL)
		t.Errorf("service pid %d survived Shutdown", pid)
	}
}

// TestShutdownOnAnEmptyManagerIsSafe pins that a zero-value ServiceManager —
// what every caller builds — tears down cleanly with no services and no guard
// pipe.
func TestShutdownOnAnEmptyManagerIsSafe(t *testing.T) {
	sm := &ServiceManager{}
	sm.Shutdown()
	sm.Shutdown()
}
