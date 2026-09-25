package testutil

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

// BuildRoot is the project build directory.
var BuildRoot = func() string {
	candidates := []string{
		"../../cmake-build-debug",
		"../../../cmake-build-debug",
		"../../build",
		"../../../build",
	}
	for _, c := range candidates {
		p := filepath.Join(c)
		if _, err := os.Stat(p); err == nil {
			abs, _ := filepath.Abs(p)
			return abs
		}
	}
	// fallback: assume called from project root
	return "build"
}()

// DataRoot is the project data directory.
var DataRoot = func() string {
	repoRoot := filepath.Clean(filepath.Join(BuildRoot, ".."))
	candidates := []string{
		filepath.Join(repoRoot, "src/content/data"),
		filepath.Join(repoRoot, "data"),
	}
	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			abs, _ := filepath.Abs(c)
			return abs
		}
	}
	return filepath.Join(repoRoot, "src/content/data")
}()

// ServiceConfig holds configuration for a single service process.
type ServiceConfig struct {
	Name       string
	Binary     string
	Args       []string
	Port       int    // 0 = ephemeral
	WorkDir    string // working directory (empty = inherit)
	ReadyCheck func(*ManagedService) bool
}

// ServiceManager manages a set of service processes for integration tests.
type ServiceManager struct {
	services []*ManagedService
}

type ManagedService struct {
	cmd     *exec.Cmd
	output  *lockedBuffer
	done    chan struct{}
	waitErr error
}

// Output returns a snapshot of everything the process has written so far.
func (s *ManagedService) Output() string {
	return s.output.String()
}

// Exited reports whether the owned process has been reaped.
func (s *ManagedService) Exited() bool {
	select {
	case <-s.done:
		return true
	default:
		return false
	}
}

// Err returns the process exit error after the child has been reaped.
func (s *ManagedService) Err() error {
	select {
	case <-s.done:
		return s.waitErr
	default:
		return nil
	}
}

type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (b *lockedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.Write(p)
}

func (b *lockedBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.String()
}

// findBinary resolves the binary path checking multiple locations.
func findBinary(name string) (string, error) {
	serviceDir := map[string]string{
		"routerd":           "message_router",
		"gatewayd":          "gateway",
		"chunkd":            "chunk_store",
		"simcored":          "simcore",
		"entitystated":      "entity_state_store",
		"metadbd":           "meta_db",
		"pipenetworkd":      "pipe_network",
		"pipe_networkd":     "pipe_network",
		"pipe_network":      "pipe_network",
		"pipe_network_exec": "pipe_network",
		"pipenetwork":       "pipe_network",
	}
	dir := name
	if mapped, ok := serviceDir[name]; ok {
		dir = mapped
	}
	repoRoot := filepath.Clean(filepath.Join(BuildRoot, ".."))
	candidates := []string{
		// Current src/apps layout.
		filepath.Join(BuildRoot, "src", "apps", dir, name),
		filepath.Join(BuildRoot, "src", "apps", dir, name+"_exec"),
		filepath.Join(repoRoot, "src", "apps", dir, name),
		// Legacy bin/ and src/services layout (kept for backward compatibility).
		filepath.Join(BuildRoot, "bin", name),
		filepath.Join(BuildRoot, name),
		filepath.Join(BuildRoot, "src", "services", dir, name),
		filepath.Join(BuildRoot, "src", "services", dir, name+"_exec"),
		filepath.Join(repoRoot, "src", "services", dir, name),
		filepath.Join(BuildRoot, "src", "services", "message_router", name),
	}
	for _, p := range candidates {
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("binary not found: %s (tried %v)", name, candidates)
}

// StartService starts a service binary and waits until it's ready. It returns
// the managed process so callers can observe application-level readiness logs.
func (sm *ServiceManager) StartService(cfg ServiceConfig) (*ManagedService, error) {
	binaryPath, err := findBinary(cfg.Binary)
	if err != nil {
		return nil, err
	}

	cmd := exec.Command(binaryPath, cfg.Args...)
	// A descendant that escapes the process group can keep the inherited
	// stdout/stderr pipe open after the direct child dies. Bound how long Wait
	// blocks on those pipes so teardown can never hang on a stray holder.
	cmd.WaitDelay = 2 * time.Second
	var output lockedBuffer
	cmd.Stdout = io.MultiWriter(os.Stdout, &output)
	cmd.Stderr = io.MultiWriter(os.Stderr, &output)
	if cfg.WorkDir != "" {
		cmd.Dir = cfg.WorkDir
	}
	cmd.Env = append(os.Environ(), "RECIPED_DATA_DIR="+DataRoot)
	// Own process group so Shutdown can SIGKILL the whole tree (wrapper
	// binaries and any children they spawn must not outlive the test run).
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start %s: %w", cfg.Name, err)
	}
	service := &ManagedService{cmd: cmd, output: &output, done: make(chan struct{})}
	sm.services = append(sm.services, service)

	// Always reap the child in one place. A process exit is terminal even if an
	// external port is still reachable; otherwise a dead owned child can look ready forever.
	exited := make(chan error, 1)
	go func() {
		err := cmd.Wait()
		service.waitErr = err
		close(service.done)
		exited <- err
	}()

	// Wait for ready.
	if cfg.ReadyCheck != nil {
		deadline := time.Now().Add(10 * time.Second)
		for time.Now().Before(deadline) {
			select {
			case err := <-exited:
				return service, fmt.Errorf("%s: exited before readiness: %w%s", cfg.Name, err, formatServiceOutput(output.String()))
			default:
			}
			if cfg.ReadyCheck(service) {
				// Recheck child health after the callback so an exit that happened
				// while checking an external port/listener cannot be accepted.
				select {
				case err := <-exited:
					return service, fmt.Errorf("%s: exited before readiness: %w%s", cfg.Name, err, formatServiceOutput(output.String()))
				default:
					if service.Exited() {
						return service, fmt.Errorf("%s: exited before readiness: %w%s", cfg.Name, service.Err(), formatServiceOutput(output.String()))
					}
					return service, nil
				}
			}
			time.Sleep(100 * time.Millisecond)
		}
		return service, fmt.Errorf("%s: not ready within 10s%s", cfg.Name, formatServiceOutput(output.String()))
	}
	return service, nil
}

func formatServiceOutput(output string) string {
	output = strings.TrimSpace(output)
	if output == "" {
		return ""
	}
	return ": " + output
}

type routerRegistration struct {
	conn    string
	service string
	topics  []string
}

func routerRegistrations(output string) []routerRegistration {
	registrations := make(map[string]routerRegistration)
	order := make([]string, 0)
	for _, line := range strings.Split(output, "\n") {
		record, ok := parseRouterLogLine(line)
		if !ok || record.conn == "" {
			continue
		}
		switch record.kind {
		case "register":
			if _, exists := registrations[record.conn]; !exists {
				order = append(order, record.conn)
			}
			registrations[record.conn] = routerRegistration{conn: record.conn, service: record.service, topics: record.topics}
		case "unregister", "cleanup":
			delete(registrations, record.conn)
		}
	}

	result := make([]routerRegistration, 0, len(order))
	for _, conn := range order {
		if registration, ok := registrations[conn]; ok {
			result = append(result, registration)
		}
	}
	return result
}

type routerLogRecord struct {
	kind    string
	conn    string
	service string
	topic   string
	topics  []string
}

func parseRouterLogLine(line string) (routerLogRecord, bool) {
	fields := strings.Fields(line)
	marker := -1
	for i, field := range fields {
		if field == "[router]" {
			marker = i
			break
		}
	}
	if marker < 0 || len(fields) < marker+3 {
		return routerLogRecord{}, false
	}

	record := routerLogRecord{kind: strings.TrimSuffix(fields[marker+1], ":")}
	for _, field := range fields[marker+2:] {
		key, value, ok := strings.Cut(field, "=")
		if !ok {
			continue
		}
		switch key {
		case "conn":
			record.conn = value
		case "service":
			record.service = value
		case "pattern":
			record.topic = value
		}
	}
	if record.kind == "register" {
		const topicsPrefix = " topics="
		if start := strings.LastIndex(line, topicsPrefix); start >= 0 {
			topicsValue := line[start+len(topicsPrefix):]
			if end := strings.IndexByte(topicsValue, ']'); end >= 0 {
				record.topics = strings.Fields(strings.TrimPrefix(topicsValue[:end], "["))
			}
		}
	}
	return record, true
}

// RouterServiceHasRegistration reports whether the Router committed a live
// registration for the exact service name.
func RouterServiceHasRegistration(output, service string) bool {
	for _, registration := range routerRegistrations(output) {
		if registration.service == service {
			return true
		}
	}
	return false
}

// RouterServiceHasRegistrationTopics reports whether the Router committed a
// live registration for the service containing every required topic.
func RouterServiceHasRegistrationTopics(output, service string, required ...string) bool {
	registrations := routerRegistrations(output)
	if len(registrations) == 0 {
		return false
	}
	for _, registration := range registrations {
		if registration.service != service {
			continue
		}
		committed := make(map[string]struct{}, len(registration.topics))
		for _, topic := range registration.topics {
			committed[topic] = struct{}{}
		}
		complete := true
		for _, topic := range required {
			if _, ok := committed[topic]; !ok {
				complete = false
				break
			}
		}
		if complete {
			return true
		}
	}
	return false
}

// RouterServiceHasSubscriptions reports whether one live Router connection has
// registered as service and committed every required subscription.
func RouterServiceHasSubscriptions(output, service string, required ...string) bool {
	type connectionState struct {
		service       string
		subscriptions map[string]struct{}
	}
	connections := make(map[string]*connectionState)
	for _, line := range strings.Split(output, "\n") {
		record, ok := parseRouterLogLine(line)
		if !ok || record.conn == "" {
			continue
		}
		switch record.kind {
		case "register":
			connections[record.conn] = &connectionState{
				service:       record.service,
				subscriptions: make(map[string]struct{}),
			}
		case "subscribe":
			if state, registered := connections[record.conn]; registered && record.topic != "" {
				state.subscriptions[record.topic] = struct{}{}
			}
		case "unsubscribe":
			if state, registered := connections[record.conn]; registered {
				delete(state.subscriptions, record.topic)
			}
		case "unregister", "cleanup":
			delete(connections, record.conn)
		}
	}

	for _, state := range connections {
		if state.service != service {
			continue
		}
		complete := true
		for _, topic := range required {
			if _, ok := state.subscriptions[topic]; !ok {
				complete = false
				break
			}
		}
		if complete {
			return true
		}
	}
	return false
}

// RouterServiceHasSubscription reports whether one Router connection has both
// registered service and committed a subscription. Matching the connection
// prevents a same-named service or subscription from an unrelated process from
// satisfying readiness.
func RouterServiceHasSubscription(output, service, topic string) bool {
	return RouterServiceHasSubscriptions(output, service, topic)
}

// Shutdown stops all managed services (process groups, so children die too).
const (
	shutdownGracePeriod = 500 * time.Millisecond
	// shutdownWaitTimeout bounds the wait for a process we already SIGKILLed.
	// It only matters when something outside our process group still holds the
	// child's inherited stdout/stderr pipe.
	shutdownWaitTimeout = 3 * time.Second
)

func (sm *ServiceManager) Shutdown() {
	for _, service := range sm.services {
		if service.cmd.Process != nil {
			_ = syscall.Kill(-service.cmd.Process.Pid, syscall.SIGINT)
		}
	}
	// Give processes time to shut down gracefully, but never wait forever on
	// a child that ignores SIGINT.
	gracefulDeadline := time.Now().Add(shutdownGracePeriod)
	for time.Now().Before(gracefulDeadline) {
		allExited := true
		for _, service := range sm.services {
			if !service.Exited() {
				allExited = false
				break
			}
		}
		if allExited {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	for _, service := range sm.services {
		if service.cmd.Process != nil {
			_ = syscall.Kill(-service.cmd.Process.Pid, syscall.SIGKILL)
		}
		select {
		case <-service.done:
		case <-time.After(shutdownWaitTimeout):
			// The reap owner is stuck on a pipe held by a process that escaped
			// our process group. Never let teardown block the test binary.
		}
	}
}
