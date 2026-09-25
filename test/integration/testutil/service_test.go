package testutil

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestStartServiceDoesNotAcceptExitedReadyChild(t *testing.T) {
	oldBuildRoot := BuildRoot
	t.Cleanup(func() { BuildRoot = oldBuildRoot })

	buildRoot := t.TempDir()
	binDir := filepath.Join(buildRoot, "bin")
	if err := os.MkdirAll(binDir, 0o755); err != nil {
		t.Fatalf("create bin directory: %v", err)
	}
	binary := filepath.Join(binDir, "exitingd")
	if err := os.WriteFile(binary, []byte("#!/bin/sh\necho exited after opening readiness surface >&2\nexit 9\n"), 0o755); err != nil {
		t.Fatalf("write fake service: %v", err)
	}
	BuildRoot = buildRoot

	sm := &ServiceManager{}
	t.Cleanup(sm.Shutdown)
	_, err := sm.StartService(ServiceConfig{
		Name:   "exitingd",
		Binary: "exitingd",
		ReadyCheck: func(service *ManagedService) bool {
			// Yield until cmd.Wait has had a chance to reap the short-lived child;
			// the callback itself intentionally reports the external surface ready.
			time.Sleep(10 * time.Millisecond)
			return true
		},
	})
	if err == nil {
		t.Fatal("StartService accepted an already-exited child")
	}
	if !strings.Contains(err.Error(), "exited before readiness") {
		t.Fatalf("StartService error = %q, want process-exit readiness error", err)
	}
}

func TestStartServiceReportsProcessExitBeforeReadiness(t *testing.T) {
	oldBuildRoot := BuildRoot
	t.Cleanup(func() { BuildRoot = oldBuildRoot })

	buildRoot := t.TempDir()
	binDir := filepath.Join(buildRoot, "bin")
	if err := os.MkdirAll(binDir, 0o755); err != nil {
		t.Fatalf("create bin directory: %v", err)
	}
	binary := filepath.Join(binDir, "failingd")
	if err := os.WriteFile(binary, []byte("#!/bin/sh\necho simulated startup failure >&2\nexit 7\n"), 0o755); err != nil {
		t.Fatalf("write fake service: %v", err)
	}
	BuildRoot = buildRoot

	sm := &ServiceManager{}
	t.Cleanup(sm.Shutdown)
	_, err := sm.StartService(ServiceConfig{
		Name:       "failingd",
		Binary:     "failingd",
		ReadyCheck: func(*ManagedService) bool { return false },
	})
	if err == nil {
		t.Fatal("StartService succeeded, want process-exit error")
	}
	if !strings.Contains(err.Error(), "exited before readiness") {
		t.Fatalf("StartService error = %q, want process-exit readiness error", err)
	}
	if !strings.Contains(err.Error(), "simulated startup failure") {
		t.Fatalf("StartService error = %q, want captured service output", err)
	}
}

func TestShutdownReturnsWhenDescendantEscapesProcessGroup(t *testing.T) {
	oldBuildRoot := BuildRoot
	t.Cleanup(func() { BuildRoot = oldBuildRoot })

	buildRoot := t.TempDir()
	binDir := filepath.Join(buildRoot, "bin")
	if err := os.MkdirAll(binDir, 0o755); err != nil {
		t.Fatalf("create bin directory: %v", err)
	}
	pidFile := filepath.Join(t.TempDir(), "escapee.pid")
	// The fake service spawns a setsid descendant that keeps the inherited
	// stdout/stderr pipe open. Group SIGINT/SIGKILL cannot reach it, so cmd.Wait
	// would never return and Shutdown must not block the test binary on it.
	script := "#!/bin/sh\nsetsid sh -c 'echo $$ > " + pidFile + "; sleep 30' &\nsleep 30\n"
	if err := os.WriteFile(filepath.Join(binDir, "escapingd"), []byte(script), 0o755); err != nil {
		t.Fatalf("write fake service: %v", err)
	}
	BuildRoot = buildRoot

	sm := &ServiceManager{}
	if _, err := sm.StartService(ServiceConfig{
		Name:   "escapingd",
		Binary: "escapingd",
	}); err != nil {
		t.Fatalf("start fake service: %v", err)
	}
	t.Cleanup(func() {
		if raw, err := os.ReadFile(pidFile); err == nil {
			if pid, err := strconv.Atoi(strings.TrimSpace(string(raw))); err == nil {
				_ = syscall.Kill(-pid, syscall.SIGKILL)
				_ = syscall.Kill(pid, syscall.SIGKILL)
			}
		}
	})
	// Give the descendant time to create its new session.
	time.Sleep(200 * time.Millisecond)

	done := make(chan struct{})
	go func() {
		sm.Shutdown()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("Shutdown did not return within 5s")
	}
}

func TestRouterServiceHasRegistration(t *testing.T) {
	tests := []struct {
		name   string
		output string
		want   bool
	}{
		{
			name:   "matching service registration",
			output: "2026/09/25 17:28:20 [router] register: conn=127.0.0.1:3003 service=metadb topics=[player.joined player.left]\n",
			want:   true,
		},
		{
			name:   "registration without service is incomplete",
			output: "[router] register: conn=127.0.0.1:3003 service= topics=[]\n",
			want:   false,
		},
		{
			name: "later registration replaces service on connection",
			output: "[router] register: conn=127.0.0.1:3003 service=metadb topics=[]\n" +
				"[router] register: conn=127.0.0.1:3003 service=other topics=[]\n",
			want: false,
		},
		{
			name: "cleanup invalidates connection",
			output: "[router] register: conn=127.0.0.1:3003 service=metadb topics=[]\n" +
				"[router] cleanup: conn=127.0.0.1:3003\n",
			want: false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := RouterServiceHasRegistration(tt.output, "metadb"); got != tt.want {
				t.Fatalf("RouterServiceHasRegistration() = %t, want %t", got, tt.want)
			}
		})
	}
}

func TestRouterServiceHasRegistrationTopics(t *testing.T) {
	tests := []struct {
		name   string
		output string
		want   bool
	}{
		{
			name:   "all required topics committed",
			output: "2026/09/25 17:28:20 [router] register: conn=127.0.0.1:2002 service=gateway topics=[metadb.player.online quest.progress.updated quest.completed.notification]\n",
			want:   true,
		},
		{
			name:   "near topic is not required topic",
			output: "[router] register: conn=127.0.0.1:2002 service=gateway topics=[metadb.player.online quest.progress.updated.v2]\n",
			want:   false,
		},
		{
			name: "later registration replaces topics",
			output: "[router] register: conn=127.0.0.1:2002 service=gateway topics=[metadb.player.online quest.progress.updated quest.completed.notification]\n" +
				"[router] register: conn=127.0.0.1:2002 service=gateway topics=[metadb.player.online]\n",
			want: false,
		},
		{
			name: "cleanup invalidates connection",
			output: "[router] register: conn=127.0.0.1:2002 service=gateway topics=[metadb.player.online quest.progress.updated quest.completed.notification]\n" +
				"[router] cleanup: conn=127.0.0.1:2002\n",
			want: false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := RouterServiceHasRegistrationTopics(tt.output, "gateway",
				"metadb.player.online", "quest.progress.updated", "quest.completed.notification")
			if got != tt.want {
				t.Fatalf("RouterServiceHasRegistrationTopics() = %t, want %t", got, tt.want)
			}
		})
	}
}

func TestRouterServiceHasSubscriptions(t *testing.T) {
	tests := []struct {
		name     string
		output   string
		required []string
		want     bool
	}{
		{
			name: "all required subscriptions committed",
			output: "[router] register: conn=127.0.0.1:2002 service=gateway topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=metadb.player.online\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=quest.progress.updated\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=quest.completed.notification\n",
			required: []string{"metadb.player.online", "quest.progress.updated", "quest.completed.notification"},
			want:     true,
		},
		{
			name: "near subscription is not required subscription",
			output: "[router] register: conn=127.0.0.1:2002 service=gateway topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=metadb.player.online\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=quest.progress.updated.v2\n",
			required: []string{"metadb.player.online", "quest.progress.updated"},
			want:     false,
		},
		{
			name: "re-registration resets subscriptions",
			output: "[router] register: conn=127.0.0.1:2002 service=gateway topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=quest.progress.updated\n" +
				"[router] register: conn=127.0.0.1:2002 service=gateway topics=[]\n",
			required: []string{"quest.progress.updated"},
			want:     false,
		},
		{
			name: "unsubscribe removes required subscription",
			output: "[router] register: conn=127.0.0.1:2002 service=gateway topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=quest.progress.updated\n" +
				"[router] unsubscribe: conn=127.0.0.1:2002 pattern=quest.progress.updated\n",
			required: []string{"quest.progress.updated"},
			want:     false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := RouterServiceHasSubscriptions(tt.output, "gateway", tt.required...); got != tt.want {
				t.Fatalf("RouterServiceHasSubscriptions() = %t, want %t", got, tt.want)
			}
		})
	}
}

func TestRouterServiceHasSubscription(t *testing.T) {
	tests := []struct {
		name   string
		output string
		want   bool
	}{
		{
			name: "matching registration and subscription",
			output: "[router] register: conn=127.0.0.1:1001 service=simcore topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:1001 pattern=quest.book.open\n",
			want: true,
		},
		{
			name:   "registration without subscription",
			output: "[router] register: conn=127.0.0.1:1001 service=simcore topics=[]\n",
			want:   false,
		},
		{
			name: "subscription belongs to another connection",
			output: "[router] register: conn=127.0.0.1:1001 service=simcore topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:2002 pattern=quest.book.open\n",
			want: false,
		},
		{
			name: "simcore suffix does not match simcore-old",
			output: "[router] register: conn=127.0.0.1:1001 service=simcore-old topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:1001 pattern=quest.book.open\n",
			want: false,
		},
		{
			name: "standard log timestamp prefix",
			output: "2026/09/25 17:28:20 [router] register: conn=127.0.0.1:1001 service=simcore topics=[]\n" +
				"2026/09/25 17:28:20 [router] subscribe: conn=127.0.0.1:1001 pattern=quest.book.open\n",
			want: true,
		},
		{
			name: "cleanup invalidates connection",
			output: "[router] register: conn=127.0.0.1:1001 service=simcore topics=[]\n" +
				"[router] cleanup: conn=127.0.0.1:1001\n" +
				"[router] register: conn=127.0.0.1:2002 service=other topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:1001 pattern=quest.book.open\n",
			want: false,
		},
		{
			name: "later registration replaces service on connection",
			output: "[router] register: conn=127.0.0.1:1001 service=simcore topics=[]\n" +
				"[router] register: conn=127.0.0.1:1001 service=other topics=[]\n" +
				"[router] subscribe: conn=127.0.0.1:1001 pattern=quest.book.open\n",
			want: false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := RouterServiceHasSubscription(tt.output, "simcore", "quest.book.open"); got != tt.want {
				t.Fatalf("RouterServiceHasSubscription() = %t, want %t", got, tt.want)
			}
		})
	}
}
