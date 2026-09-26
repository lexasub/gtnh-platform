package testutil

// Supervisor (service shim).
//
// A Go test binary cannot guarantee it runs cleanup: `os.Exit` skips defers,
// the `-test.timeout` deadline panics from a *different* goroutine, and
// SIGKILL gives the process no chance at all. So teardown cannot be left to
// the parent. It has to be driven by something the kernel keeps running when
// the parent is gone.
//
// The supervisor is that something. For every service the harness starts, it
// re-execs the test binary in supervisor mode and hands it the real service
// binary to run:
//
//	test binary (parent)                supervisor (re-exec of this binary)
//	 ├─ holds the write end of a pipe   ├─ starts the service
//	 │                                   │   (same process group as itself)
//	 │                                   └─ blocks reading the guard pipe
//	 └─ exits / dies / is SIGKILLed ────▶ read returns EOF
//	                                        └─ SIGKILL its own process group
//
// The parent holds the write end, so the kernel closes it however the parent
// dies (os.Exit, timeout panic, SIGKILL, abort). The supervisor learns about
// the parent's death from a kernel event rather than a signal it would have
// to be alive to receive. This is why every init system pairs a supervised
// child with a pipe or socketpair instead of trusting SIGTERM.
//
// The re-exec is dispatched from init() so both consumers of testutil — the
// integration suite and the testutil unit tests — get a working supervisor
// without each package opting in from its own TestMain. os.Exit in init() is
// deliberate: a supervisor must not start the test machinery.
//
// Note that PR_SET_PDEATHSIG would be the obvious alternative and does not
// work here: the kernel clears it across execve (verified against this
// kernel), and the service is always reached through an exec.

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"time"
)

const (
	// supervisorEnv marks a re-exec of the test binary as a supervisor.
	// A plausible service argv is also required, so an unrelated process that
	// merely inherits this variable cannot turn into a supervisor.
	supervisorEnv = "GTNH_INTEGRATION_SERVICE_SUPERVISOR"
	// os/exec hands ExtraFiles to the child starting at fd 3, in order.
	supervisorGuardFD  = 3 // read end of the run's guard pipe
	supervisorLaunchFD = 4 // write end of the one-shot launch handshake
)

func init() {
	if os.Getenv(supervisorEnv) != "1" || len(os.Args) < 2 {
		return
	}
	// argv is [<service binary> <service args...>].
	if info, err := os.Stat(os.Args[1]); err != nil || info.IsDir() {
		return
	}
	runSupervisor(os.Args[1], os.Args[2:])
}

// Launch report, written by the supervisor to the handshake pipe. The parent
// reads two lines: the service pid, then the status it exited with. The second
// line is what lets the parent tell a dead service from a live one — it cannot
// use kill(pid, 0) on its own, because an unreaped child is a zombie and a
// zombie still exists.
const (
	launchOKPrefix = "ok "
	// launchSettleTimeout bounds how long the parent waits for the handshake.
	// The supervisor writes the first line immediately after fork, so anything
	// slower means the supervisor never got as far as trying.
	launchSettleTimeout = 5 * time.Second
	// exitReportSettle bounds how long the parent waits for the supervisor's
	// exit report before concluding the service is still alive. A healthy
	// service writes nothing, so this is a small fixed wait: long enough to
	// cover a service that failed instantly, short enough not to slow startup.
	exitReportSettle = 250 * time.Millisecond
)

func writeLaunch(handshake *os.File, pid int) {
	_, _ = fmt.Fprintf(handshake, "%s%d\n", launchOKPrefix, pid)
}

func reportLaunchFailure(handshake *os.File, err error) {
	_, _ = fmt.Fprintf(handshake, "err %v\n", err)
}

func reportExit(handshake *os.File, status int) {
	_, _ = fmt.Fprintf(handshake, "exit %d\n", status)
}

// runSupervisor starts the service and guarantees it cannot outlive the test
// binary. It never returns: it exits with the service's status.
//
// The launch handshake is deliberate. Without it, a service that fails to exec
// looks identical to one that is merely slow, because the parent only observes
// the supervisor, which stays alive to run the watchdog. The parent would then
// accept a readiness signal that an external surface happened to satisfy and
// conclude a broken service is healthy.
func runSupervisor(binary string, args []string) {
	handshake := os.NewFile(supervisorLaunchFD, "launch-pipe")
	if handshake == nil {
		fmt.Fprintln(os.Stderr, "supervisor: launch pipe missing, refusing to start service")
		os.Exit(126)
	}
	// The parent's copy must not leak into the service's own children.
	defer handshake.Close()

	cmd := exec.Command(binary, args...)
	// Inherit the supervisor's own stdio (an *os.File, so os/exec passes the
	// descriptors through without creating pipes). The parent captured the
	// supervisor's fd 1/2, so service output still reaches the test log.
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	// Stay in the supervisor's process group. The parent created that group
	// and kills the whole group; a service in any other group would be
	// unreachable by both of the kill paths that matter.
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "supervisor: start %s: %v\n", binary, err)
		// Report the failure through the handshake, then leave the same
		// status the parent would have seen from a direct exec failure.
		reportLaunchFailure(handshake, err)
		os.Exit(127)
	}
	// Tell the parent the service is running. Best effort: if the parent is
	// already gone the write fails and the watchdog is about to fire anyway.
	writeLaunch(handshake, cmd.Process.Pid)

	// Watchdog: the parent vanished. Nothing else can tell us, because the
	// parent may have been SIGKILLed.
	watchdogDone := make(chan struct{})
	go func() {
		defer close(watchdogDone)
		guard := os.NewFile(supervisorGuardFD, "guard-pipe")
		if guard == nil {
			return
		}
		defer guard.Close()
		// Any read error, EOF in particular, means the write end is gone from
		// every process that held it: the parent is finished.
		var scratch [1]byte
		for {
			if _, err := guard.Read(scratch[:]); err != nil {
				break
			}
		}
		// Kill first, and say nothing. This runs precisely when the parent is
		// gone, which means our inherited stdout/stderr are pipes with no
		// reader; a write to them raises SIGPIPE, whose default action kills
		// this process before it reaches the kill below. A diagnostic that
		// cancels the cleanup it is describing is worse than no diagnostic —
		// and the service it was meant to explain then leaks.
		//
		// Both kills are needed. The service may have escaped this process
		// group (a wrapper script that calls setsid, for instance), in which
		// case only the direct kill reaches it; the group kill is what stops
		// the supervisor itself from lingering.
		_ = cmd.Process.Kill()
		_ = syscall.Kill(-syscall.Getpgrp(), syscall.SIGKILL)
	}()

	err := cmd.Wait()
	// Report the service's exit. The parent cannot work this out for itself:
	// the service is this process's child, so it lingers as a zombie until we
	// reap it, and a zombie still answers `kill(pid, 0)`. Without this line
	// the parent would see a dead service as a live one.
	reportExit(handshake, exitStatus(err))
	// Never block on the watchdog. On the normal path the parent killed the
	// whole group and this process is already dead; on the leak path the
	// watchdog has SIGKILLed us. Reaching this select at all means the guard
	// pipe somehow stayed open, so fall through and report the status.
	select {
	case <-watchdogDone:
	case <-time.After(supervisorOrphanGrace):
		fmt.Fprintln(os.Stderr, "supervisor: guard pipe still open after service exit, exiting")
	}
	os.Exit(exitStatus(err))
}

// supervisorOrphanGrace bounds the wait for the watchdog after the service
// itself has exited. Nothing should be left to clean up at that point.
const supervisorOrphanGrace = 500 * time.Millisecond

func exitStatus(err error) int {
	if err == nil {
		return 0
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		return exitErr.ExitCode()
	}
	return 1
}
