package testutil

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

// BuildRoot is the project build directory.
var BuildRoot = func() string {
	candidates := []string{
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
	candidates := []string{
		"../../data",
		"../../../data",
	}
	for _, c := range candidates {
		p := filepath.Join(c)
		if _, err := os.Stat(p); err == nil {
			abs, _ := filepath.Abs(p)
			return abs
		}
	}
	return "data"
}()

// ServiceConfig holds configuration for a single service process.
type ServiceConfig struct {
	Name       string
	Binary     string
	Args       []string
	Port       int // 0 = ephemeral
	WorkDir    string // working directory (empty = inherit)
	ReadyCheck func() bool
}

// ServiceManager manages a set of service processes for integration tests.
type ServiceManager struct {
	cmds []*exec.Cmd
}

// findBinary resolves the binary path checking multiple locations.
func findBinary(name string) (string, error) {
	candidates := []string{
		filepath.Join(BuildRoot, "bin", name),
		filepath.Join(BuildRoot, name),
		filepath.Join(BuildRoot, "src", "services", name, name),
		filepath.Join(BuildRoot, "src", "services", name, name+"_exec"),
	}
	for _, p := range candidates {
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("binary not found: %s (tried %v)", name, candidates)
}

// StartService starts a service binary and waits until it's ready.
func (sm *ServiceManager) StartService(cfg ServiceConfig) error {
	binaryPath, err := findBinary(cfg.Binary)
	if err != nil {
		return err
	}

	cmd := exec.Command(binaryPath, cfg.Args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if cfg.WorkDir != "" {
		cmd.Dir = cfg.WorkDir
	}
	cmd.Env = append(os.Environ(), "RECIPED_DATA_DIR="+DataRoot)

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start %s: %w", cfg.Name, err)
	}
	sm.cmds = append(sm.cmds, cmd)

	// Wait for ready
	if cfg.ReadyCheck != nil {
		deadline := time.Now().Add(10 * time.Second)
		for time.Now().Before(deadline) {
			if cfg.ReadyCheck() {
				return nil
			}
			time.Sleep(100 * time.Millisecond)
		}
		return fmt.Errorf("%s: not ready within 10s", cfg.Name)
	}
	return nil
}

// Shutdown stops all managed services.
func (sm *ServiceManager) Shutdown() {
	for _, cmd := range sm.cmds {
		if cmd.Process != nil {
			cmd.Process.Signal(os.Interrupt)
		}
	}
	// Give processes time to shut down gracefully
	time.Sleep(500 * time.Millisecond)
	for _, cmd := range sm.cmds {
		if cmd.Process != nil {
			cmd.Process.Kill()
			cmd.Wait()
		}
	}
}
