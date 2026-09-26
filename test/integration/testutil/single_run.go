package testutil

import (
	"strconv"
	"sync"
)

var (
	singleRunMu   sync.Mutex
	singleRunSeen = map[string]int{}
)

// ClaimSingleRun records that name has been executed once in this process and
// returns an error when it is claimed again.
//
// TestMain starts one process-scoped service cluster and every Ctrl connection
// is assigned the same player, so a focused tracer cannot prove its cause
// twice: quest state already recorded on the first run suppresses the second
// run's completion notification. Repeating such a test inside one process
// fails later with an unrelated read timeout, which reads like a transport
// bug. Claiming the run turns that into an explicit, actionable contract.
func ClaimSingleRun(name string) error {
	singleRunMu.Lock()
	defer singleRunMu.Unlock()

	singleRunSeen[name]++
	if singleRunSeen[name] == 1 {
		return nil
	}
	return &SingleRunError{Name: name, Run: singleRunSeen[name]}
}

// SingleRunError reports a repeat attempt of a process-scoped test.
type SingleRunError struct {
	Name string
	Run  int
}

func (e *SingleRunError) Error() string {
	return "test " + e.Name + " is single-run per process: run #" + strconv.Itoa(e.Run) +
		" started in a process that already ran it; " +
		"TestMain serves one process-scoped cluster and every Ctrl connection gets the same player, " +
		"so server state from the first run suppresses the second run's notifications; " +
		"rerun with -count=1 (one run per process) or start a fresh test process"
}
