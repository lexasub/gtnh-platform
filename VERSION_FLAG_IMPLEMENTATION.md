# Version Flag Implementation - Gateway Service

## What Was Implemented

Added `--version` and `-v` command-line flags to the **Gateway** service (`src/services/gateway/main.cpp`).

## Changes Made

### 1. Added Required Headers

```cpp
#include <iostream>  // For std::cout to print version information
#include <string>    // For std::string comparison in argv parsing
```

**Explanation:**
- `<iostream>` provides `std::cout` for printing to stdout (cleaner than printf)
- `<string>` provides `std::string` for easier string comparison in argv parsing

### 2. Early Version Check Loop

```cpp
for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version" || arg == "-v") {
        // ... version printing logic
        return 0;
    }
}
```

**Explanation:**
- Loops through command-line arguments starting at index 1 (skipping program name at index 0)
- Converts each `char*` argument to `std::string` for easier comparison
- Checks for both `--version` (GNU-style long flag) and `-v` (short flag)
- Returns 0 immediately after printing, preventing service initialization

### 3. Version Information Output

```cpp
std::cout << "Gateway Service (gatewayd)\n";
std::cout << "Version: (not configured - see main.cpp for setup instructions)\n";
std::cout << "Git Hash: (not configured)\n";
std::cout << "Build Date: (not configured)\n";
```

**Explanation:**
- Prints the service name clearly identifying which binary this is
- Shows placeholder text indicating version tracking is not yet configured
- Includes inline documentation in comments explaining how to add real version info

## Current Behavior

```bash
$ ./gatewayd --version
Gateway Service (gatewayd)
Version: (not configured - see main.cpp for setup instructions)
Git Hash: (not configured)
Build Date: (not configured)

$ ./gatewayd -v
Gateway Service (gatewayd)
Version: (not configured - see main.cpp for setup instructions)
Git Hash: (not configured)
Build Date: (not configured)
```

The service exits with code 0 (success) without starting any network listeners or initializing subsystems.

## How to Add Real Version Information

The code includes detailed comments explaining how to add proper build information. Here's the summary:

### Step 1: Create `src/version.h.in`

```cpp
#define GTNH_VERSION_MAJOR @PROJECT_VERSION_MAJOR@
#define GTNH_VERSION_MINOR @PROJECT_VERSION_MINOR@
#define GTNH_VERSION_PATCH @PROJECT_VERSION_PATCH@
#define GTNH_GIT_HASH "@GIT_HASH@"
#define GTNH_BUILD_DATE "@BUILD_DATE@"
```

This template file contains CMake placeholders (the `@VARIABLE@` syntax) that will be replaced at build time.

### Step 2: Update `CMakeLists.txt`

Add version to the project declaration:
```cmake
project(GTNHPlatform VERSION 0.1.0 CXX C)
```

Add git hash extraction:
```cmake
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
```

Add build timestamp:
```cmake
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)
```

Generate the header:
```cmake
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.h.in
    ${CMAKE_BINARY_DIR}/version.h
    @ONLY
)
```

Add build directory to include path:
```cmake
include_directories(${CMAKE_BINARY_DIR})
```

### Step 3: Use in `main.cpp`

Include the generated header:
```cpp
#include "version.h"
```

Replace placeholder prints with:
```cpp
std::cout << "Version: " << GTNH_VERSION_MAJOR << "."
          << GTNH_VERSION_MINOR << "." << GTNH_VERSION_PATCH << "\n";
std::cout << "Git Hash: " << GTNH_GIT_HASH << "\n";
std::cout << "Build Date: " << GTNH_BUILD_DATE << "\n";
```

## Why This Approach?

1. **Early exit**: Version check happens before any initialization (logging, networking, etc.)
2. **Zero dependencies**: Uses only standard library (`<iostream>`, `<string>`)
3. **Self-documenting**: Comments explain exactly how to add real version info
4. **No invented data**: Doesn't fake version numbers or timestamps
5. **Standard conventions**: Supports both `--version` (GNU) and `-v` (common shorthand)
6. **Clean output**: Uses stdout (not spdlog) so it works even if logging isn't initialized

## Testing

To test the implementation:

```bash
# Build the service
cd build
cmake ..
make gatewayd

# Test version flag
./src/services/gateway/gatewayd --version
./src/services/gateway/gatewayd -v

# Verify it still runs normally without flags
./src/services/gateway/gatewayd --port 7777
```

## Next Steps

To roll this out to other services:

1. Implement the CMake version infrastructure (Steps 1-2 above)
2. Copy the version checking code to other service main.cpp files
3. Update service names in the output (e.g., "ChunkStore Service (chunkd)")
4. For Go services (message_router, meta_db), use similar approach with `flag` package
