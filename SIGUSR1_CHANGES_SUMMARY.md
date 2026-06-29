# SIGUSR1 Implementation Summary - Gateway Service

## What Changed

### Files Modified: 1
- `src/services/gateway/main.cpp`

### Lines Added: ~40
- 1 new global variable
- 1 new signal handler function
- 1 signal registration call
- 1 start time capture
- ~35 lines for metrics printing in main loop

---

## Visual Diff

### Change 1: Add Metrics Request Flag

**Location:** Line 19 (after `g_running` declaration)

```diff
  static std::atomic<bool> g_running{true};
+ static std::atomic<bool> g_print_metrics{false};    // Metrics request flag (set by SIGUSR1)
```

**Explanation:**
- Adds second atomic boolean flag
- Used to communicate from signal handler to main loop
- `false` = no metrics requested
- `true` = SIGUSR1 received, print metrics

---

### Change 2: Add SIGUSR1 Handler

**Location:** Lines 31-40 (after `handleSignal` function)

```diff
  extern "C" void handleSignal([[maybe_unused]] int sig) {
      g_running.store(false, std::memory_order_release);
  }
  
+ extern "C" void handleSIGUSR1([[maybe_unused]] int sig) {
+     // Just set a flag - the actual printing happens in the main loop
+     // This is the safe pattern: signal handler sets flag, main loop handles it
+     g_print_metrics.store(true, std::memory_order_release);
+ }
```

**Explanation:**
- Creates new signal handler function for SIGUSR1
- `extern "C"` ensures C linkage (required for signal handlers)
- Only sets atomic flag (async-signal-safe)
- Does NOT print directly (would be unsafe!)

---

### Change 3: Register SIGUSR1 Handler

**Location:** Line 99 (in signal registration section)

```diff
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);
  std::signal(SIGPIPE, SIG_IGN);
+ std::signal(SIGUSR1, handleSIGUSR1);
```

**Explanation:**
- Tells OS: "When SIGUSR1 arrives, call handleSIGUSR1"
- Without this, SIGUSR1 would terminate the process (default behavior)
- Now SIGUSR1 triggers our custom behavior (print metrics)

---

### Change 4: Capture Start Time

**Location:** Line 106 (after signal registration)

```diff
  std::signal(SIGPIPE, SIG_IGN);
+ std::signal(SIGUSR1, handleSIGUSR1);
+ 
+ // ── Record startup time ───────────────────────────────────────────────
+ // steady_clock is monotonic (doesn't jump if system time changes)
+ // We'll use this to calculate uptime when SIGUSR1 is received
+ const auto start_time = std::chrono::steady_clock::now();
  
  IoUringGateway gateway;
```

**Explanation:**
- Records moment when service starts
- Uses `steady_clock` (monotonic, won't jump if user changes system time)
- `const` because start time never changes
- Used later to calculate uptime (now - start_time)

---

### Change 5: Check Flag and Print Metrics

**Location:** Lines 154-186 (at start of main loop)

```diff
  while (g_running) {
+     // ── Check for metrics request (SIGUSR1) ───────────────────────────
+     if (g_print_metrics.load(std::memory_order_acquire)) {
+         g_print_metrics.store(false, std::memory_order_release);
+         
+         // Calculate uptime
+         auto now = std::chrono::steady_clock::now();
+         auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
+             now - start_time).count();
+         
+         // Format uptime as days, hours, minutes, seconds
+         int days = uptime_seconds / 86400;
+         int hours = (uptime_seconds % 86400) / 3600;
+         int minutes = (uptime_seconds % 3600) / 60;
+         int seconds = uptime_seconds % 60;
+         
+         // Print metrics using spdlog
+         spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
+         spdlog::info("METRICS: Gateway Service (gatewayd)");
+         spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
+         spdlog::info("Uptime: {} days, {:02d}:{:02d}:{:02d}", days, hours, minutes, seconds);
+         spdlog::info("Control Port: {}", ctrl_port);
+         spdlog::info("Bulk Port: {}", bulk_port);
+         spdlog::info("Router Port: {}", router_port);
+         spdlog::info("Client Connected: {}", gateway.has_client() ? "yes" : "no");
+         spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
+     }
+     
      static auto lastHb = std::chrono::steady_clock::now();
```

**Explanation:**
- Checks if metrics flag was set (by signal handler)
- If true: reset flag, calculate uptime, print metrics
- If false: skip (normal operation)
- Happens in main loop (safe to use logging, malloc, etc.)
- Main loop polls every 1 second, so metrics appear within 1 second of SIGUSR1

---

## Code Flow Diagram

```
                                    ┌─────────────────────────┐
                                    │  Terminal/Script/User   │
                                    └───────────┬─────────────┘
                                                │
                                    kill -SIGUSR1 <pid>
                                                │
                                                ▼
┌────────────────────────────────────────────────────────────────┐
│  Signal Handler (Async Context - Limited Operations)          │
│                                                                │
│  extern "C" void handleSIGUSR1(int sig) {                     │
│      g_print_metrics.store(true);  ◄── SET FLAG ONLY          │
│  }                                                             │
└────────────────────────────────────────────────────────────────┘
                                                │
                                                │ Signal handler returns
                                                │ Program resumes
                                                ▼
┌────────────────────────────────────────────────────────────────┐
│  Main Loop (Safe Context - All Operations Allowed)            │
│                                                                │
│  while (g_running) {                                           │
│      if (g_print_metrics.load()) {  ◄── CHECK FLAG            │
│          g_print_metrics.store(false);                         │
│          // Calculate uptime                                   │
│          // Print metrics with spdlog  ◄── SAFE HERE!         │
│      }                                                         │
│      // ... normal work ...                                    │
│      sleep(1 second);                                          │
│  }                                                             │
└────────────────────────────────────────────────────────────────┘
```

---

## Example Usage

### Terminal 1: Start Gateway
```bash
$ ./gatewayd --port 7777
[14:23:10.123] [info] Gateway starting: router=localhost:4000 ctrl=:7777 bulk=:7778
[14:23:10.234] [info] Gateway running — worker thread handles io_uring
```

### Terminal 2: Find PID
```bash
$ ps aux | grep gatewayd
user  12345  0.1  0.5  ./gatewayd --port 7777

# Or use pidof:
$ pidof gatewayd
12345
```

### Terminal 2: Send SIGUSR1
```bash
$ kill -SIGUSR1 12345
```

### Terminal 1: Metrics Appear
```bash
[14:23:15.456] [info] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[14:23:15.456] [info] METRICS: Gateway Service (gatewayd)
[14:23:15.456] [info] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[14:23:15.456] [info] Uptime: 0 days, 00:00:05
[14:23:15.456] [info] Control Port: 7777
[14:23:15.456] [info] Bulk Port: 7778
[14:23:15.456] [info] Router Port: 4000
[14:23:15.456] [info] Client Connected: no
[14:23:15.456] [info] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[14:23:15.456] [info] Gateway running — worker thread handles io_uring
```

Service continues running normally!

---

## Metrics Displayed

All metrics shown are **real data** already tracked by the service:

| Metric | Source | Type |
|--------|--------|------|
| Service Name | Hardcoded | Static |
| Uptime | `now - start_time` | Calculated |
| Control Port | `ctrl_port` variable | Configuration |
| Bulk Port | `bulk_port` variable | Configuration |
| Router Port | `router_port` variable | Configuration |
| Client Connected | `gateway.has_client()` | Runtime state |

**No invented metrics!** Everything comes from existing code.

---

## Key Design Decisions

### ✅ Async-Signal-Safe
Signal handler only sets atomic flag - no logging, no malloc, no locks.

### ✅ Flag-and-Poll Pattern
Industry-standard approach for signal handling:
1. Handler sets flag (fast, safe)
2. Main loop checks flag (safe context)
3. Main loop does actual work (can use any functions)

### ✅ Non-Intrusive
- Main loop structure unchanged
- Service continues normally
- Zero performance impact when not triggered
- Minimal performance impact when triggered (~1ms for printing)

### ✅ Memory Ordering
Uses appropriate memory ordering:
- `release` in handler (ensures write visible)
- `acquire` in main loop (ensures read sees write)

### ✅ Real Data Only
Shows only metrics already maintained by service:
- Ports from command-line arguments
- Connection status from gateway object
- Uptime calculated from start time

---

## What Happens When SIGUSR1 is Sent

### Millisecond-by-Millisecond:

**t=0ms:** User types `kill -SIGUSR1 12345`

**t=5ms:** OS delivers SIGUSR1 to process
- Process is interrupted (might be in sleep, might be processing network data)
- OS saves process state
- OS calls `handleSIGUSR1`

**t=5.1ms:** `handleSIGUSR1` executes
- Sets `g_print_metrics` to `true`
- Returns immediately (microseconds)

**t=5.2ms:** Process resumes normal execution
- Continues from wherever it was interrupted
- No visible interruption

**t=0-1000ms later:** Main loop checks flag
- Main loop wakes from sleep (runs every 1 second)
- Checks `if (g_print_metrics.load())`
- Flag is `true`!

**t=1000ms + ~1ms:** Metrics printed
- Calculates uptime
- Formats output
- Logs with spdlog
- Resets flag to `false`

**t=1001ms onwards:** Normal operation continues

---

## Technical Details

### Atomic Operations
```cpp
std::atomic<bool> g_flag{false};

// In signal handler:
g_flag.store(true, std::memory_order_release);
// Ensures: All prior writes are visible before this write

// In main loop:
if (g_flag.load(std::memory_order_acquire)) {
// Ensures: This read sees all writes that happened before the store
```

### Why `steady_clock`?
```cpp
system_clock::now()  // ❌ Jumps if user changes time
                     //    Uptime would be wrong!

steady_clock::now()  // ✅ Monotonic, never jumps
                     //    Perfect for intervals
```

### Time Calculation
```cpp
auto start = steady_clock::now();
// ... service runs ...
auto now = steady_clock::now();
auto duration = now - start;  // Type: duration
auto seconds = duration_cast<seconds>(duration).count();  // Type: int64_t
```

---

## Common Questions

### Q: Why not print directly in signal handler?

**A:** Signal handlers can interrupt ANY code:
```cpp
// Main thread might be here:
printf("Hello");  // Holding lock on stdout buffer

// Signal arrives, handler tries to print:
printf("Metrics");  // Tries to get same lock → DEADLOCK!
```

### Q: Why atomic instead of regular bool?

**A:** Race conditions:
```cpp
// Non-atomic (WRONG):
bool flag = false;

// Thread 1:           // Thread 2:
flag = true;           if (flag) { ... }
// Might not see write!

// Atomic (CORRECT):
std::atomic<bool> flag{false};

// Thread 1:           // Thread 2:
flag.store(true);      if (flag.load()) { ... }
// Guaranteed to see write!
```

### Q: What if SIGUSR1 is sent twice quickly?

**A:** No problem! Flag stays `true` until checked:
```cpp
// User sends SIGUSR1 at t=0
g_print_metrics = true;

// User sends SIGUSR1 at t=0.1s (before check)
g_print_metrics = true;  // Still true, no change

// Main loop checks at t=1s
if (g_print_metrics.load()) {  // True
    g_print_metrics.store(false);
    printMetrics();  // Prints once
}
```

Result: Metrics printed once (as expected).

### Q: Can I use SIGUSR2 for something else?

**A:** Yes! Same pattern:
```cpp
static std::atomic<bool> g_reset_counters{false};

extern "C" void handleSIGUSR2(int) {
    g_reset_counters.store(true);
}

int main() {
    std::signal(SIGUSR2, handleSIGUSR2);
    
    while (running) {
        if (g_reset_counters.load()) {
            g_reset_counters.store(false);
            // Reset statistics...
        }
    }
}
```

---

## Summary

**What we built:** On-demand metrics printing via SIGUSR1

**How it works:** Signal handler sets flag → main loop prints

**Why it's safe:** Handler only touches atomic flag (no locks, malloc, or logging)

**What it shows:** Service name, uptime, ports, client connection status

**Impact:** Zero when not triggered, minimal when triggered

**Pattern:** Standard flag-and-poll (industry best practice)

This implementation can be replicated to all other services with minor adjustments for service-specific metrics!
