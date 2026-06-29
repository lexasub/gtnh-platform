# SIGUSR1 Signal Handler Implementation - Gateway Service

## What is SIGUSR1?

SIGUSR1 is a **user-defined signal** in Unix/Linux systems that allows external processes to send messages to your running program without terminating it.

### Signal Types in Unix:
- **SIGINT** (Ctrl+C) - Interrupt from keyboard
- **SIGTERM** (kill command) - Termination request
- **SIGKILL** (kill -9) - Force kill (cannot be caught)
- **SIGUSR1** - User-defined signal 1 (we choose what it does)
- **SIGUSR2** - User-defined signal 2

---

## How Signals Work

```
Terminal                      Your Program
--------                      ------------
                              while (running) {
$ kill -SIGUSR1 1234  ───>       // Program pauses here
                              }
                              
                              ↓ OS interrupts execution
                              
                              handleSIGUSR1() {
                                  // Your handler runs
                              }
                              
                              ↓ Returns to main loop
                              
                              while (running) {
                                  // Continues here
                              }
```

### Key Points:
1. **Asynchronous** - Handler can interrupt at ANY point in your code
2. **Cannot be prevented** - Your code WILL be interrupted
3. **Limited operations** - Only async-signal-safe functions allowed
4. **Must be fast** - Blocking operations can deadlock

---

## Signal Handler Safety

### ❌ UNSAFE in Signal Handlers:
```cpp
void unsafeHandler(int sig) {
    printf("Got signal!\n");        // ❌ Uses malloc internally
    std::cout << "Signal\n";        // ❌ Uses locks
    spdlog::info("Signal");         // ❌ Uses locks and malloc
    malloc(100);                    // ❌ Can deadlock
    std::string s = "test";         // ❌ Allocates memory
    throw std::runtime_error("x");  // ❌ Exceptions not safe
}
```

**Why unsafe?** Imagine:
1. Main thread is inside `malloc()`, holding a lock
2. Signal arrives, interrupts main thread
3. Handler calls `printf()` which calls `malloc()`
4. `malloc()` tries to acquire lock... but main thread has it!
5. **DEADLOCK** 💀

### ✅ SAFE in Signal Handlers:
```cpp
std::atomic<bool> g_flag{false};  // Atomic is lock-free

void safeHandler(int sig) {
    g_flag.store(true);  // ✅ Atomic operation
    write(2, "Signal received\n", 16);  // ✅ async-signal-safe syscall
}
```

---

## The Flag-and-Poll Pattern (What We Use)

This is the standard safe approach:

```cpp
// 1. Global atomic flag
std::atomic<bool> g_print_metrics{false};

// 2. Signal handler just sets flag
extern "C" void handleSIGUSR1(int) {
    g_print_metrics.store(true);  // Fast, safe, lock-free
}

// 3. Main loop checks flag
int main() {
    signal(SIGUSR1, handleSIGUSR1);
    
    while (running) {
        if (g_print_metrics.load()) {  // Check in safe context
            g_print_metrics.store(false);
            printMetrics();  // Safe here! Can use malloc, logging, etc.
        }
        // ... normal work
    }
}
```

### Why This Works:
- Signal handler does MINIMAL work (just set flag)
- Actual work happens in main loop (safe context)
- Atomic operations are lock-free (no deadlock risk)
- Main loop polls at regular intervals (1 second in our case)

---

## Implementation Breakdown

### 1. Global Atomic Flags

```cpp
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_print_metrics{false};
```

**What it does:**
- Declares two global variables visible to entire file
- `static` means file-scope only (not visible to other .cpp files)
- `std::atomic<bool>` provides thread-safe boolean operations
- `{true}` and `{false}` are initializers

**Why needed:**
- Signal handlers can't access local variables (they run in different context)
- Atomic ensures thread-safe access (no race conditions)
- `g_running` controls main loop (set by SIGINT/SIGTERM)
- `g_print_metrics` triggers metrics printing (set by SIGUSR1)

**What happens if removed:**
- Signal handlers can't communicate with main thread
- Race conditions possible (main thread and handler both accessing variable)

**C++ Concepts:**
- **`static`** - File-scope linkage
- **`std::atomic<T>`** - Lock-free atomic operations
- **Uniform initialization** - `{value}` syntax

---

### 2. Signal Handler for SIGUSR1

```cpp
extern "C" void handleSIGUSR1([[maybe_unused]] int sig) {
    g_print_metrics.store(true, std::memory_order_release);
}
```

**Line-by-line:**

#### `extern "C"`
- **What**: Tells compiler to use C linkage (no name mangling)
- **Why**: `signal()` function expects a C function pointer
- **What happens if removed**: Linker error - C++ mangles function names

**C++ Concept:** Name mangling
```cpp
// C++ mangles function names to support overloading:
void foo(int);      // Mangled to: _Z3fooi
void foo(double);   // Mangled to: _Z3food

// extern "C" prevents mangling:
extern "C" void foo(int);  // Not mangled: just "foo"
```

#### `void handleSIGUSR1`
- **What**: Function returns nothing (`void`)
- **Why**: Signal handlers must return void

#### `[[maybe_unused]] int sig`
- **What**: Parameter attribute suppressing "unused parameter" warning
- **Why**: Signal handlers must accept `int` (signal number) but we don't need it
- **What happens if removed**: Compiler warning (harmless but noisy)

**C++ Concept:** Attributes (C++17)
```cpp
[[maybe_unused]] int x;  // Suppress "unused variable" warning
[[nodiscard]] int foo(); // Warn if return value ignored
[[deprecated]] void old(); // Warn that function is deprecated
```

#### `g_print_metrics.store(true, ...)`
- **What**: Atomically sets flag to true
- **Why**: Signal handler needs to communicate with main thread
- **What happens if removed**: No metrics printed when SIGUSR1 received

#### `std::memory_order_release`
- **What**: Memory ordering constraint for atomic operation
- **Why**: Ensures changes are visible to other threads
- **What happens if removed**: Defaults to `memory_order_seq_cst` (stronger, slower)

**C++ Concept:** Memory ordering
```cpp
// Weakest to strongest:
std::memory_order_relaxed  // No synchronization
std::memory_order_acquire  // Prevents reordering of subsequent reads
std::memory_order_release  // Prevents reordering of prior writes
std::memory_order_acq_rel  // Both acquire and release
std::memory_order_seq_cst  // Strongest, sequential consistency (default)
```

**For our case:**
- `release` in handler: Ensures flag write happens before handler returns
- `acquire` in main loop: Ensures we see the flag write

---

### 3. Register Signal Handler

```cpp
std::signal(SIGUSR1, handleSIGUSR1);
```

**What it does:**
- Registers `handleSIGUSR1` as handler for SIGUSR1 signal
- Returns previous handler (we ignore it)

**Why needed:**
- Without this, SIGUSR1 uses default action (terminate process!)
- This tells OS: "When SIGUSR1 arrives, call my function"

**What happens if removed:**
- Sending SIGUSR1 terminates the process (not what we want!)

**C++ Concept:** Function pointer
```cpp
void myHandler(int);                    // Function declaration
std::signal(SIGUSR1, myHandler);       // Pass function as pointer
std::signal(SIGUSR1, &myHandler);      // Explicit address-of (same thing)
```

---

### 4. Record Start Time

```cpp
const auto start_time = std::chrono::steady_clock::now();
```

**Line-by-line:**

#### `const`
- **What**: Variable cannot be modified after initialization
- **Why**: Start time never changes
- **What happens if removed**: Works but allows accidental modification

#### `auto`
- **What**: Compiler deduces type from initializer
- **Why**: Type is complex: `std::chrono::time_point<std::chrono::steady_clock>`
- **What happens if removed**: Must write full type (verbose!)

**C++ Concept:** Type deduction (C++11)
```cpp
// Without auto (verbose):
std::chrono::time_point<std::chrono::steady_clock> start = std::chrono::steady_clock::now();

// With auto (concise):
auto start = std::chrono::steady_clock::now();
```

#### `start_time`
- **What**: Variable name
- **Why**: Descriptive name for readability

#### `std::chrono::steady_clock::now()`
- **What**: Gets current time from monotonic clock
- **Why**: For uptime calculation

**C++ Concept:** `std::chrono` clocks
```cpp
// Three clock types:
system_clock  // System time (can jump if user changes clock)
steady_clock  // Monotonic (always increases, never jumps) ← We use this
high_resolution_clock  // Highest precision available
```

**Why `steady_clock`?**
- Won't jump if user changes system time
- Perfect for measuring elapsed time (uptime)

---

### 5. Main Loop - Check Flag

```cpp
if (g_print_metrics.load(std::memory_order_acquire)) {
    g_print_metrics.store(false, std::memory_order_release);
    // ... print metrics ...
}
```

**Line-by-line:**

#### `if (g_print_metrics.load(...))`
- **What**: Atomically reads flag value
- **Why**: Check if SIGUSR1 was received
- **What happens if removed**: Metrics never printed

#### `std::memory_order_acquire`
- **What**: Memory ordering for atomic read
- **Why**: Ensures we see the write from signal handler
- **What happens if removed**: Defaults to stronger ordering (works but slower)

#### `g_print_metrics.store(false, ...)`
- **What**: Reset flag to false
- **Why**: So we only print once per SIGUSR1 (not every loop iteration)
- **What happens if removed**: Metrics printed every second forever!

---

### 6. Calculate Uptime

```cpp
auto now = std::chrono::steady_clock::now();
auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
    now - start_time).count();
```

**Line-by-line:**

#### `auto now = std::chrono::steady_clock::now()`
- **What**: Get current time
- **Why**: To compare with start time

#### `now - start_time`
- **What**: Subtracts two time points, returns duration
- **Why**: Calculate time elapsed

**C++ Concept:** `std::chrono` arithmetic
```cpp
time_point - time_point = duration
time_point + duration = time_point
duration + duration = duration
```

#### `std::chrono::duration_cast<std::chrono::seconds>(...)`
- **What**: Converts duration to seconds
- **Why**: Duration might be nanoseconds, we want seconds
- **What happens if removed**: Can't call `.count()` on raw duration

**C++ Concept:** Duration types
```cpp
std::chrono::nanoseconds   // 1/1,000,000,000 second
std::chrono::microseconds  // 1/1,000,000 second
std::chrono::milliseconds  // 1/1,000 second
std::chrono::seconds       // 1 second
std::chrono::minutes       // 60 seconds
std::chrono::hours         // 3600 seconds
```

#### `.count()`
- **What**: Extracts integer value from duration
- **Why**: Get actual number (e.g., 3600) instead of type-safe duration object

---

### 7. Format Uptime

```cpp
int days = uptime_seconds / 86400;
int hours = (uptime_seconds % 86400) / 3600;
int minutes = (uptime_seconds % 3600) / 60;
int seconds = uptime_seconds % 60;
```

**What it does:** Converts total seconds to days/hours/minutes/seconds

**Math breakdown:**
- 1 day = 86400 seconds (60 × 60 × 24)
- 1 hour = 3600 seconds (60 × 60)
- 1 minute = 60 seconds

**Example:** `uptime_seconds = 90061` (25 hours, 1 minute, 1 second)
```cpp
days = 90061 / 86400 = 1        // 1 full day
hours = (90061 % 86400) / 3600  // 3661 / 3600 = 1 hour
      = 3661 / 3600 = 1
minutes = (90061 % 3600) / 60   // 61 / 60 = 1 minute
        = 61 / 60 = 1
seconds = 90061 % 60 = 1        // 1 second
```

**Result:** "1 days, 01:01:01"

**C++ Concept:** Integer division and modulo
```cpp
10 / 3 = 3    // Integer division (truncates)
10 % 3 = 1    // Modulo (remainder)

// Pattern: extract components from total
total_seconds / 60 = minutes
total_seconds % 60 = remaining seconds
```

---

### 8. Print Metrics

```cpp
spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
spdlog::info("METRICS: Gateway Service (gatewayd)");
spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
spdlog::info("Uptime: {} days, {:02d}:{:02d}:{:02d}", days, hours, minutes, seconds);
spdlog::info("Control Port: {}", ctrl_port);
spdlog::info("Bulk Port: {}", bulk_port);
spdlog::info("Router Port: {}", router_port);
spdlog::info("Client Connected: {}", gateway.has_client() ? "yes" : "no");
spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
```

**What it does:** Prints metrics in formatted box

**Line-by-line:**

#### `spdlog::info(...)`
- **What**: Logs at INFO level
- **Why**: Standard logging (not debug, not error)

#### `"{}"`
- **What**: Placeholder for format argument
- **Why**: spdlog uses Python-style formatting
- **What happens if removed**: Literal `{}` printed

#### `"{:02d}"`
- **What**: Format spec - decimal number, width 2, zero-padded
- **Why**: Shows "01" instead of "1" (cleaner time format)
- **Examples:**
  - `{:02d}` with 1 → "01"
  - `{:02d}` with 15 → "15"
  - `{:02d}` with 5 → "05"

**C++ Concept:** Format specifications
```cpp
{}      // Default format
{:d}    // Decimal integer
{:02d}  // Decimal, width 2, zero-padded
{:x}    // Hexadecimal
{:f}    // Floating point
{:.2f}  // Floating point, 2 decimal places
```

#### `gateway.has_client() ? "yes" : "no"`
- **What**: Ternary operator (conditional expression)
- **Why**: Convert boolean to string
- **What happens if removed**: Would print "1" or "0" (not user-friendly)

**C++ Concept:** Ternary operator
```cpp
condition ? value_if_true : value_if_false

// Equivalent to:
if (condition)
    return value_if_true;
else
    return value_if_false;
```

---

## Metrics Shown (All Already Available)

1. **Service Name**: Hardcoded
2. **Uptime**: Calculated from `start_time`
3. **Control Port**: `ctrl_port` variable
4. **Bulk Port**: `bulk_port` variable
5. **Router Port**: `router_port` variable
6. **Client Connected**: `gateway.has_client()` method

**No invented metrics** - all data already exists in the program!

---

## How to Use

### Start the service:
```bash
$ ./gatewayd --port 7777
[12:34:56.123] [info] Gateway starting: router=localhost:4000 ctrl=:7777 bulk=:7778
[12:34:56.234] [info] Gateway running — worker thread handles io_uring
```

### Find the process ID:
```bash
$ ps aux | grep gatewayd
user     12345  0.1  0.5  gatewayd
```

### Send SIGUSR1:
```bash
$ kill -SIGUSR1 12345
```

### Output appears in logs:
```
[12:35:01.456] [info] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[12:35:01.456] [info] METRICS: Gateway Service (gatewayd)
[12:35:01.456] [info] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[12:35:01.456] [info] Uptime: 0 days, 00:00:05
[12:35:01.456] [info] Control Port: 7777
[12:35:01.456] [info] Bulk Port: 7778
[12:35:01.456] [info] Router Port: 4000
[12:35:01.456] [info] Client Connected: no
[12:35:01.456] [info] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

## Why This Design is Good

1. ✅ **Async-signal-safe** - Handler only sets atomic flag
2. ✅ **No deadlocks** - No locks, malloc, or logging in handler
3. ✅ **Non-blocking** - Main loop polls flag every second
4. ✅ **Real data** - No invented metrics
5. ✅ **Doesn't interfere** - Main loop logic unchanged
6. ✅ **Standard pattern** - Flag-and-poll is industry best practice

---

## Common Pitfalls Avoided

### ❌ Don't do this:
```cpp
extern "C" void badHandler(int sig) {
    spdlog::info("Metrics...");  // DEADLOCK RISK!
}
```

### ✅ Do this instead:
```cpp
std::atomic<bool> g_flag{false};

extern "C" void goodHandler(int sig) {
    g_flag.store(true);  // Safe!
}

int main() {
    while (running) {
        if (g_flag.load()) {
            spdlog::info("Metrics...");  // Safe here!
        }
    }
}
```

---

## Summary

**What we added:**
1. Atomic flag `g_print_metrics`
2. Signal handler `handleSIGUSR1` that sets the flag
3. Registration: `std::signal(SIGUSR1, handleSIGUSR1)`
4. Start time capture for uptime calculation
5. Flag check in main loop that prints metrics

**What we didn't change:**
- Main loop structure (still 1-second sleep)
- Gateway initialization
- Network handling
- Any existing functionality

**Result:**
- Send SIGUSR1 → metrics appear in logs
- Service continues running normally
- Zero performance impact
- Safe, standard, maintainable
