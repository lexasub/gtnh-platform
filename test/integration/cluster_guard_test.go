package integration

import (
	"net"
	"strconv"
	"strings"
	"testing"
)

// TestRequirePortsFreeDetectsALeakedCluster covers the guard that turns gp-pnok's
// worst symptom — a leaked cluster holding :5001 — into a loud failure.
//
// The old harness started anyway, printed "SKIP: chunkd not available", and ran
// every test against a cluster with no ChunkStore, so the run produced
// "read tcp ...:7777: i/o timeout" and could still report passes.
func TestRequirePortsFreeDetectsALeakedCluster(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("bind a listener: %v", err)
	}
	defer listener.Close()
	_, portStr, err := net.SplitHostPort(listener.Addr().String())
	if err != nil {
		t.Fatalf("split listener address: %v", err)
	}
	port, err := strconv.Atoi(portStr)
	if err != nil {
		t.Fatalf("parse listener port %q: %v", portStr, err)
	}

	err = requirePortsFree([]int{port})
	if err == nil {
		t.Fatal("requirePortsFree accepted a port that is already bound")
	}
	// The message has to name the port and say what to do, because this is
	// what a person reads when a run refuses to start.
	if !strings.Contains(err.Error(), ":"+portStr) {
		t.Errorf("error %q does not name the busy port", err)
	}
	if !strings.Contains(err.Error(), "pgrep") {
		t.Errorf("error %q does not say how to find the leaked cluster", err)
	}
}

func TestRequirePortsFreeAcceptsFreePorts(t *testing.T) {
	// Pick two ports nothing is listening on by binding and releasing them.
	first, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("bind a listener: %v", err)
	}
	_, firstStr, _ := net.SplitHostPort(first.Addr().String())
	first.Close()

	second, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("bind a listener: %v", err)
	}
	_, secondStr, _ := net.SplitHostPort(second.Addr().String())
	second.Close()

	firstPort, _ := strconv.Atoi(firstStr)
	secondPort, _ := strconv.Atoi(secondStr)

	if err := requirePortsFree([]int{firstPort, secondPort}); err != nil {
		t.Fatalf("requirePortsFree rejected free ports %d and %d: %v", firstPort, secondPort, err)
	}
}
