package testutil

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
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

	// The single guard pipe. guardR is the read end handed to every supervisor;
	// guardW is the write end the parent holds open for the whole run. The
	// kernel closes guardW however the parent dies (os.Exit, `-test.timeout`
	// panic, SIGKILL), each supervisor sees EOF, and each kills its own service
	// process group. That is the only mechanism that survives SIGKILL.
	guardOnce sync.Once
	guardR    *os.File
	guardW    *os.File
	guardErr  error

	// Shutdown runs at most once; later callers wait for the first to finish.
	// A WaitGroup rather than a channel so a zero-value ServiceManager (every
	// caller builds one with &ServiceManager{}) works without initialisation.
	shutdownOnce sync.Once
	shutdownDone sync.WaitGroup
}

// guardPipe returns the read end of the run's guard pipe, creating it on first
// use. Every supervisor gets the same read end, so a single held-open write end
// keeps all of them alive until Shutdown (or the parent's death) closes it.
func (sm *ServiceManager) guardPipe() (*os.File, error) {
	sm.guardOnce.Do(func() {
		sm.guardR, sm.guardW, sm.guardErr = os.Pipe()
	})
	return sm.guardR, sm.guardErr
}

type ManagedService struct {
	cmd     *exec.Cmd
	output  *lockedBuffer
	done    chan struct{}
	waitErr error

	// Handshake state, written by the two reader goroutines StartService
	// launches and read by the test goroutine. Guarded by mu: the exit report
	// arrives whenever the service dies, which is exactly when a test is
	// asking whether it is still alive.
	mu              sync.Mutex
	servicePID      int
	launchErr       error
	serviceErr      error
	launchedCh      chan struct{}
	serviceExitedCh chan struct{}
	launched        atomic.Bool
	serviceDone     atomic.Bool
}

// setLaunch records the outcome of the launch handshake.
func (s *ManagedService) setLaunch(pid int, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.servicePID = pid
	s.launchErr = err
}

// setServiceExit records the outcome the supervisor reported when it reaped
// the service.
func (s *ManagedService) setServiceExit(err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.serviceErr = err
}

// snapshot returns the handshake state in one atomic read, so a caller never
// sees a pid from one report paired with an error from another.
func (s *ManagedService) snapshot() (pid int, launchErr, serviceErr error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.servicePID, s.launchErr, s.serviceErr
}

// ServicePID is the pid of the supervised service, or 0 if it never started.
func (s *ManagedService) ServicePID() int {
	pid, _, _ := s.snapshot()
	return pid
}

// readLaunch consumes the supervisor's launch report and records the service
// pid. The second line (the service's exit status) is read afterwards by
// readExit. launchedCh is always closed, so every waiter is released whether
// the supervisor reported success, a failure, or nothing at all.
func (s *ManagedService) readLaunch(launchR *os.File) {
	defer close(s.launchedCh)

	// The supervisor writes one short line and then waits. A deadline is belt
	// and braces: if it died before writing, the read ends at EOF anyway, but
	// a wedged supervisor must not wedge this goroutine.
	_ = launchR.SetReadDeadline(time.Now().Add(launchSettleTimeout))
	reader := bufio.NewReader(launchR)
	line, err := reader.ReadString('\n')
	switch {
	case err != nil && line == "":
		// No report. Leave launchErr nil: the supervisor may have exited
		// before writing, and awaitLaunch decides whether that is fatal
		// using the supervisor's own exit status.
		s.setLaunch(0, nil)
	case strings.HasPrefix(line, launchOKPrefix):
		pid, convErr := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, launchOKPrefix)))
		if convErr != nil {
			s.setLaunch(0, fmt.Errorf("supervisor reported an unreadable service pid %q", strings.TrimSpace(line)))
		} else {
			s.setLaunch(pid, nil)
		}
	case strings.HasPrefix(line, "err "):
		s.setLaunch(0, fmt.Errorf("supervisor could not start the service: %s", strings.TrimSpace(strings.TrimPrefix(line, "err "))))
	default:
		s.setLaunch(0, fmt.Errorf("supervisor sent an unrecognised launch report %q", strings.TrimSpace(line)))
	}
	s.launched.Store(true)
	// Keep the buffered reader: the exit line is still queued behind the
	// launch line in the same pipe.
	go s.readExit(launchR, reader)
}

// readExit waits for the supervisor's exit report and records that the service
// is gone. It owns the launch pipe from here: readLaunch must not close it, or
// this read fails with "file already closed" and the service is never reported.
//
// A supervisor that dies at all closes its end of the pipe, which ends this
// read, so there is no deadline and no way for this goroutine to be pinned.
func (s *ManagedService) readExit(launchR *os.File, reader *bufio.Reader) {
	defer launchR.Close()
	line, err := reader.ReadString('\n')
	status, convErr := serviceExitStatus(line)
	switch {
	case err != nil && line == "":
		// No exit line at all: the supervisor died without reporting. Leave
		// serviceErr nil and do not set serviceDone, so Exited falls back to
		// the process table. Setting it here would report a service that is
		// still running as gone.
		return
	case convErr != nil:
		s.setServiceExit(fmt.Errorf("supervisor sent an unrecognised exit report %q", strings.TrimSpace(line)))
	default:
		s.setServiceExit(fmt.Errorf("service exited with status %d: %w", status, errServiceExited))
	}
	s.serviceDone.Store(true)
	close(s.serviceExitedCh)
}

func serviceExitStatus(line string) (int, error) {
	if !strings.HasPrefix(line, "exit ") {
		return 0, errServiceExited
	}
	return strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, "exit ")))
}

// errServiceExited marks the report the supervisor sends when the service it
// was running has terminated. Its content is not interesting; the fact that it
// exists is the signal.
var errServiceExited = errors.New("service exited")

// awaitLaunch blocks until the supervisor reports that the service is running,
// and returns an error if the service could not be started. Readiness must not
// be accepted before this succeeds: the supervisor stays alive even when the
// service it runs fails, so its own lifetime proves nothing about the service.
func (s *ManagedService) awaitLaunch(timeout time.Duration) error {
	_, launchErr, _ := s.snapshot()
	if s.launched.Load() {
		return launchErr
	}
	if launchErr != nil {
		return launchErr
	}
	select {
	case <-s.launchedCh:
		_, launchErr, _ = s.snapshot()
		return launchErr
	case <-s.done:
		// The supervisor exited without reporting. If it failed to exec the
		// service, that is the reason; otherwise it died in a way the parent
		// can only report generically.
		_, launchErr, _ = s.snapshot()
		if launchErr == nil && s.waitErr != nil {
			return fmt.Errorf("supervisor exited before starting the service: %w", s.waitErr)
		}
		return launchErr
	case <-time.After(timeout):
		return fmt.Errorf("supervisor did not report a service start within %s", timeout)
	}
}

// processAlive reports whether a pid still names a live, un-reaped process.
func processAlive(pid int) bool {
	// Signal 0 performs the permission and existence checks without
	// delivering anything. A reaped child leaves an ESRCH here, which is what
	// makes this a liveness test rather than a zombie test.
	return syscall.Kill(pid, 0) == nil
}

// Output returns a snapshot of everything the process has written so far.
func (s *ManagedService) Output() string {
	return s.output.String()
}

// Exited reports whether the owned processes are gone: the supervisor has been
// reaped, and so has the service it was running.
//
// The service is the one that matters. A supervisor deliberately outlives a
// service that has already exited — it stays alive to run the watchdog — so
// treating the supervisor's exit as "the service exited" would let a dead
// service look healthy for the rest of the run, and would let Shutdown return
// early while the service is still holding a port.
//
// The supervisor's exit report is authoritative. kill(pid, 0) is not: the
// service is the supervisor's child, so an unreaped one is a zombie and a
// zombie still answers signal 0 as a live process.
func (s *ManagedService) Exited() bool {
	if s.serviceDone.Load() {
		return true
	}
	select {
	case <-s.serviceExitedCh:
		return true
	case <-s.done:
		// The supervisor is gone. Fall back to the process table, which is
		// authoritative here precisely because the supervisor has reaped (or
		// been killed alongside) the service.
		pid, _, _ := s.snapshot()
		return pid == 0 || !processAlive(pid)
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
//
// The service does not run as a direct child: the test binary re-execs itself
// as a supervisor (see supervisor.go) and the supervisor runs the service. The
// guard pipe the supervisor watches is what makes the cluster die even when
// this process is SIGKILLed or dies to `-test.timeout`.
func (sm *ServiceManager) StartService(cfg ServiceConfig) (*ManagedService, error) {
	binaryPath, err := findBinary(cfg.Binary)
	if err != nil {
		return nil, err
	}
	self, err := os.Executable()
	if err != nil {
		return nil, fmt.Errorf("resolve test binary for supervisor: %w", err)
	}

	// One guard pipe for the whole run, not one per service: a pipe whose
	// write end is closed reads as EOF immediately, so a per-service pipe
	// would tell the second supervisor its parent was gone the moment it
	// started.
	guardR, err := sm.guardPipe()
	if err != nil {
		return nil, fmt.Errorf("guard pipe for %s: %w", cfg.Name, err)
	}

	// Re-exec this test binary as the supervisor; it runs the real service.
	cmd := exec.Command(self, append([]string{binaryPath}, cfg.Args...)...)
	// A descendant that escapes the process group can keep the inherited
	// stdout/stderr pipe open after the direct child dies. Bound how long Wait
	// blocks on those pipes so teardown can never hang on a stray holder.
	cmd.WaitDelay = 2 * time.Second
	cmd.Env = append(os.Environ(), supervisorEnv+"=1", "RECIPED_DATA_DIR="+DataRoot)
	launchR, launchW, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("launch pipe for %s: %w", cfg.Name, err)
	}
	// fd 3 is the guard pipe (see guardPipe); the supervisor reads the
	// handshake from fd 4.
	cmd.ExtraFiles = []*os.File{guardR, launchW}
	var output lockedBuffer
	cmd.Stdout = io.MultiWriter(os.Stdout, &output)
	cmd.Stderr = io.MultiWriter(os.Stderr, &output)
	if cfg.WorkDir != "" {
		cmd.Dir = cfg.WorkDir
	}
	// Own process group so Shutdown can SIGKILL the whole tree (the
	// supervisor and the service it runs must not outlive the test run).
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}

	if err := cmd.Start(); err != nil {
		launchR.Close()
		launchW.Close()
		return nil, fmt.Errorf("start %s: %w", cfg.Name, err)
	}
	// The supervisor owns the write end now. If the parent kept it open, EOF
	// would never arrive, so close it before anyone waits on the handshake.
	launchW.Close()
	service := &ManagedService{
		cmd:             cmd,
		output:          &output,
		done:            make(chan struct{}),
		launchedCh:      make(chan struct{}),
		serviceExitedCh: make(chan struct{}),
	}
	sm.services = append(sm.services, service)

	// Read the launch handshake exactly once, off the critical path.
	go service.readLaunch(launchR)

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
				return service, service.readinessError(cfg.Name, "exited before readiness", err)
			default:
			}
			if cfg.ReadyCheck(service) {
				// A service that is ready must be a service that started, so
				// confirm the supervisor actually launched it before
				// accepting readiness. The ReadyCheck cannot do this: it
				// inspects external surfaces (a port, a Router log line), all
				// of which can look ready while the supervised service is
				// still on its way up — or has already failed and left them
				// satisfiable.
				launchErr := service.awaitLaunch(launchSettleTimeout)
				if launchErr != nil {
					return service, fmt.Errorf("%s: %w%s", cfg.Name, launchErr, formatServiceOutput(output.String()))
				}
				// A service that is already gone cannot be ready, however
				// ready the surface it left behind looks. The supervisor
				// reaps the service and writes its exit report, so wait for
				// that report to arrive before believing the ReadyCheck:
				// otherwise a check that returns true instantly (or one
				// satisfied by a port an unrelated process still holds)
				// outruns it and accepts a dead service.
				select {
				case <-service.serviceExitedCh:
					_, _, serviceErr := service.snapshot()
					return service, service.readinessError(cfg.Name, "exited before readiness", serviceErr)
				case <-time.After(exitReportSettle):
					// No exit report: the service is still running.
				}
				// Recheck child health after the callback so an exit that happened
				// while checking an external port/listener cannot be accepted.
				select {
				case err := <-exited:
					return service, service.readinessError(cfg.Name, "exited before readiness", err)
				default:
					if service.Exited() {
						// Prefer the supervisor's own account of the service
						// over the supervisor's exit status: a supervisor that
						// outlives its service exits 0, which says nothing.
						_, _, serviceErr := service.snapshot()
						return service, service.readinessError(cfg.Name, "exited before readiness", serviceErr)
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

// readinessError builds the "this service died before it was ready" error.
// Either cause may legitimately be nil — a service can exit 0 before readiness
// — so the status is always spelled out rather than wrapped, because
// fmt.Errorf("%w", nil) renders as %!w(<nil>) instead of saying anything.
func (s *ManagedService) readinessError(name, reason string, cause error) error {
	detail := ""
	switch {
	case errors.Is(cause, errServiceExited):
		detail = cause.Error()
	case cause != nil:
		detail = cause.Error()
	default:
		detail = "the process exited without reporting a status"
	}
	return fmt.Errorf("%s: %s (%s)%s", name, reason, detail, formatServiceOutput(s.output.String()))
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
//
// It is idempotent and bounded: the exit paths call it from a signal handler
// and from a normal return, sometimes both, and teardown must never be the
// thing that hangs. The guard pipe is closed first so that any supervisor
// still alive is released even if a later kill cannot be delivered.
const (
	shutdownGracePeriod = 500 * time.Millisecond
	// shutdownWaitTimeout bounds the wait for a process we already SIGKILLed.
	// It only matters when something outside our process group still holds the
	// child's inherited stdout/stderr pipe.
	shutdownWaitTimeout = 3 * time.Second
)

func (sm *ServiceManager) Shutdown() {
	sm.shutdownOnce.Do(func() {
		// Release the supervisors first. A service whose group signal is
		// undeliverable (already reaped, or blocked) still dies when it sees
		// EOF on the guard pipe, so this must not wait on the kills below.
		if sm.guardW != nil {
			sm.guardW.Close()
		}
		sm.killAll()
	})
	// Concurrent callers (signal handler plus the deferred teardown) block
	// until the one that ran the body is done, then return.
	sm.shutdownDone.Wait()
}

func (sm *ServiceManager) killAll() {
	sm.shutdownDone.Add(1)
	defer sm.shutdownDone.Done()
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
		// Also target the service itself. Its supervisor may already have been
		// reaped, in which case the group signal above goes to a pgid nobody
		// belongs to any more and the service would be left holding its port.
		if pid := service.ServicePID(); pid != 0 {
			_ = syscall.Kill(pid, syscall.SIGKILL)
		}
		select {
		case <-service.done:
		case <-time.After(shutdownWaitTimeout):
			// The reap owner is stuck on a pipe held by a process that escaped
			// our process group. Never let teardown block the test binary.
		}
	}
	// Last resort: the guard pipe is already closed, so any supervisor that is
	// still alive is running its watchdog. Wait briefly for the services to
	// actually go away so a caller that immediately re-binds the same ports is
	// not racing a dying process.
	deadline := time.Now().Add(shutdownWaitTimeout)
	for time.Now().Before(deadline) {
		alive := false
		for _, service := range sm.services {
			if pid := service.ServicePID(); pid != 0 && processAlive(pid) {
				alive = true
				break
			}
		}
		if !alive {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
}
