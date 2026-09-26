package testutil

import (
	"errors"
	"fmt"
	"strings"
	"sync/atomic"
	"testing"
)

var claimSeq atomic.Int64

// claimName gives each invocation of a unit test its own claim key so these
// tests stay repeatable: testutil has no process-scoped cluster, so a fixed key
// would make the second -count run of this file fail for the wrong reason.
func claimName(t *testing.T) string {
	t.Helper()
	return fmt.Sprintf("%s#%d", t.Name(), claimSeq.Add(1))
}

func TestClaimSingleRunAllowsTheFirstRun(t *testing.T) {
	if err := ClaimSingleRun(claimName(t)); err != nil {
		t.Fatalf("first claim rejected: %v", err)
	}
}

func TestClaimSingleRunRejectsARepeatWithGuidance(t *testing.T) {
	name := claimName(t)

	if err := ClaimSingleRun(name); err != nil {
		t.Fatalf("first claim rejected: %v", err)
	}
	err := ClaimSingleRun(name)
	if err == nil {
		t.Fatal("second claim returned nil, want SingleRunError")
	}
	var singleRun *SingleRunError
	if !errors.As(err, &singleRun) {
		t.Fatalf("second claim error type = %T, want *SingleRunError", err)
	}
	if singleRun.Name != name {
		t.Fatalf("SingleRunError.Name = %q, want %q", singleRun.Name, name)
	}
	if singleRun.Run != 2 {
		t.Fatalf("SingleRunError.Run = %d, want 2", singleRun.Run)
	}
	for _, want := range []string{"single-run per process", "-count=1", "player"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("error %q does not mention %q", err.Error(), want)
		}
	}
}

func TestClaimSingleRunCountsEveryRepeat(t *testing.T) {
	name := claimName(t)

	if err := ClaimSingleRun(name); err != nil {
		t.Fatalf("first claim rejected: %v", err)
	}
	for run := 2; run <= 4; run++ {
		err := ClaimSingleRun(name)
		var singleRun *SingleRunError
		if err == nil {
			t.Fatalf("claim #%d returned nil, want SingleRunError", run)
		}
		if !errors.As(err, &singleRun) {
			t.Fatalf("claim #%d error type = %T, want *SingleRunError", run, err)
		}
		if singleRun.Run != run {
			t.Fatalf("claim #%d reports run %d", run, singleRun.Run)
		}
	}
}
