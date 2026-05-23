# SPEC-gtnh-core (DEPRECATED)

> **Superseded by `doc/SPEC-gtnh-libs.md`** — вместо одного монолитного `gtnh_core` используем набор мелких статических библиотек (`gtnh_types`, `gtnh_registry`, `gtnh_pipegraph`, etc.). Каждая lib линкуется только куда нужно.

# GTNH Core Library Specification (Old)

## Overview

This document defines the `gtnh_core` library - a stable, test-driven library extracted from the GTNH Platform project. It contains core components that are data-driven and stable, suitable for long-term use across multiple services.

## 1. Library Composition

### 1.1 MachineRegistry
**Location:** `src/libs/machine_registry/` (to be moved to `src/core/`)

**Purpose:** Provides data-driven machine definitions from CSV and DB sources. Replaces hardcoded machine block detection.

**Current API:**
- `Load(const char* consumers_path, const char* producers_path)` - Load machine definitions
- `Get(uint16_t block_id)` - Get machine info by ID
- `IsMachine(uint16_t block_id)` - Check if block is a machine
- `IsConsumer(uint16_t block_id)` / `IsProducer(uint16_t block_id)` - Check machine role
- `All()` - Get all machine definitions
- `EnergyLabel(EnergyType et)` / `EnergyTypeToString(EnergyType et)` - Helper functions

**Data Files:**
- `data/registry/consumers.csv` (8 machines: heat_furnace, heat_macerator, steam_macerator, steam_compressor, bronze_alloy_smelter, steam_extractor, steam_mixer)
- `data/registry/producers.csv` (5 machines: heat_generator, steam_solid_boiler, steam_heat_boiler, creative_generator)

### 1.2 ECS Core Components
**Current Location:** `src/services/simulation_core/ECS/components/`

**To Extract:** Only stable, data-driven components:

#### 1.2.1 EnergyStorage.h
- Fields: capacity, current, maxInput, maxOutput, tier, energy_type
- Methods: addEnergy(), consumeEnergy(), isFull(), isEmpty()
- **Stable:** Yes - pure data container with helper functions

#### 1.2.2 Position.h
- Fields: x, y, z
- **Stable:** Yes - immutable position data

#### 1.2.3 InventoryContainer.h
- Fields: entity_type, slot_count, slots vector
- Methods: getSlot(), setSlot(), addItem(), removeItem()
- **Stable:** Yes - slot management logic

#### 1.2.4 RecipeProgress.h (TO BE EXCLUDED)
- Contains processing state - **unstable**, orchestration logic

#### 1.2.5 MachineComponent.h (TO BE EXCLUDED)
- Contains managed_externally flag - **unstable**, orchestration logic

#### 1.2.6 MultiblockController.h (TO BE EXCLUDED)
- Multiblock orchestration - **unstable**

#### 1.2.7 BiomeComponent.h (TO BE EXCLUDED)
- Environment-dependent - **unstable**

#### 1.2.8 NetworkConnectionComponent.h (TO BE EXCLUDED)
- Network topology - **unstable**

### 1.3 FlatBuffers Schema Validation
**New Utility:** SchemaValidator

**Purpose:** Validates FlatBuffers schemas at build time to ensure:
- All .fbs files parse correctly
- GatewayPayload union types 0..11 have struct definitions
- Enum values have no duplicates
- Vec3i used with correct dimension

**Files to Validate:**
- `src/protocol/core.fbs`
- `src/protocol/gateway.fbs`
- `src/protocol/chunk.fbs`
- `src/protocol/simcore.fbs`
- `src/protocol/recipe.fbs`
- `src/protocol/entity_state_store.fbs`
- `src/protocol/pipe_network.fbs`
- `src/protocol/tile_entity_store.fbs`
- `src/protocol/item_registry.fbs`

### 1.4 PipeNetwork Core
**Location:** `src/services/pipe_network/` (extract graph algorithms)

**To Extract:** Core graph algorithms:

#### 1.4.1 EnergyNode / PipeEdge
- Fields: id, position, energy/fluid buffers, source/sink flags
- **Stable:** Yes - pure graph topology

#### 1.4.2 BFS Graph Construction
- `discoverNetwork(startNodeId)` - Find connected components
- `rebuildNetworks()` - Rebuild all networks
- **Stable:** Yes - graph topology algorithms

#### 1.4.3 distributeEnergy() Algorithm
- `distributeEnergy(networkId, tickEnergy)` - Distribute energy across network
- **Stable:** Yes - energy distribution algorithm

**TO BE EXCLUDED:**
- MessageRouterClient (service-specific)
- PipeNetworkService (service layer)

### 1.5 EnergyType / Energy Enums
**Location:** `src/protocol/core.fbs`

**Enums to Extract:**
- `EnergyType` (ELECTRICITY, HEAT, STEAM)
- `Voltage tier constants` (ULV..UHV from MachineRegistry)
- Conversion helpers (EnergyLabel, EnergyTypeToString)

## 2. Tests

### 2.1 MachineRegistryTest
**Test Cases:**
1. `ParseConsumersCSV()` - Load 8 consumers from consumers.csv
2. `ParseProducersCSV()` - Load 5 producers from producers.csv
3. `GetMachineInfo(36)` - Returns heat_furnace, CONSUMER, HEAT
4. `GetMachineInfo(999)` - Returns nullptr (invalid ID)
5. `IsConsumer(48)` - Returns true (heat_macerator)
6. `IsProducer(46)` - Returns true (heat_generator)

### 2.2 ProtocolSchemaTest
**Test Cases:**
1. `ValidateAllSchemas()` - Each .fbs file parses with flatbuffers::Parser
2. `GatewayPayloadTypes()` - Union types 0..11 all have struct definitions
3. `PlayerActionTypeNoDuplicates()` - Enum values unique
4. `Vec3iUsage()` - All Vec3i used with correct dimension (int32)

### 2.3 PipeNetworkCoreTest
**Test Cases:**
1. `BuildGraphConnected()` - 3 nodes, 2 edges → connected
2. `BuildGraphDisconnected()` - Disconnected nodes → separate networks
3. `DistributeEnergyFair()` - 1 source, 2 consumers → fair split
4. `DistributeEnergyCapped()` - Consumer with max limit → capped

### 2.4 ComponentSerializationTest
**Test Cases:**
1. `EnergyStorageRoundtrip()` - EnergyStorage → FlatBuffer → EnergyStorage
2. `PositionSerialization()` - Position serialization/deserialization
3. `InventoryContainerSlots()` - Slots vector serialization

## 3. Directory Structure

```
src/
├── core/                          ← NEW: gtnh_core library
│   ├── CMakeLists.txt
│   ├── include/gtnh/core/
│   │   ├── MachineRegistry.h
│   │   ├── EnergyStorage.h
│   │   ├── Position.h
│   │   ├── RecipeProgress.h          ← TO BE EXCLUDED
│   │   ├── InventoryContainer.h
│   │   ├── PipeNetworkGraph.h       ← BFS core
│   │   ├── EnergyTypes.h            ← enums + constants
│   │   └── SchemaValidator.h        ← FlatBuffers validation
│   └── src/
│       ├── MachineRegistry.cpp
│       ├── PipeNetworkGraph.cpp
│       └── SchemaValidator.cpp
├── core/tests/                    ← tests
│   ├── CMakeLists.txt
│   ├── MachineRegistryTest.cpp
│   ├── PipeNetworkCoreTest.cpp
│   ├── ComponentSerializationTest.cpp
│   └── SchemaValidationTest.cpp
├── services/
│   ├── simulation_core/   (remains, depends on gtnh_core)
│   ├── pipe_network/      (remains, depends on gtnh_core)
│   └── ...
└── protocol/              (remains on place)
```

## 4. Build System

### 4.1 gtnh_core Library CMakeLists.txt

```cmake
add_library(gtnh_core STATIC
    src/MachineRegistry.cpp
    src/PipeNetworkGraph.cpp
    src/SchemaValidator.cpp
)

target_include_directories(gtnh_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(gtnh_core PUBLIC
    flatbuffers::flatbuffers
    fmt::fmt                    # MachineRegistry uses fmt
)

target_link_libraries(gtnh_core PRIVATE
    fast_noise_lite::fast_noise_lite
)

# Install targets
install(TARGETS gtnh_core
    EXPORT gtnh_coreTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/gtnh/core/
    DESTINATION include/gtnh/core
    FILES_MATCHING PATTERN "*.h"
)
```

### 4.2 Tests CMakeLists.txt

```cmake
add_executable(gtnh_core_test
    tests/MachineRegistryTest.cpp
    tests/PipeNetworkCoreTest.cpp
    tests/ComponentSerializationTest.cpp
    tests/SchemaValidationTest.cpp
)

target_link_libraries(gtnh_core_test PRIVATE
    gtnh_core
    Catch2::Catch2
    Catch2::Catch2WithMain
)

# Add test to CTest
add_test(NAME gtnh_core_test COMMAND gtnh_core_test)

# Coverage reporting (optional)
if(BUILD_COVERAGE)
    target_compile_options(gtnh_core_test PRIVATE --coverage)
    target_link_options(gtnh_core_test PRIVATE --coverage)
endif()
```

## 5. Migration Plan

### Phase 1: MachineRegistry + Tests (2 hours)
1. Create `core/` directory and CMakeLists.txt
2. Copy `MachineRegistry.h/.cpp` from `libs/` to `core/`
3. Write `MachineRegistryTest.cpp` with all 6 test cases
4. Update `simulation_core` to depend on `gtnh_core` instead of `libs/machine_registry`
5. Replace `isMachineBlock()` in `SimulationEngine` with `MachineRegistry::getMachineInfo()`
6. Replace `defaultMachineSlotCount()` with registry lookup

### Phase 2: ECS Components + Serialization Tests (1 day)
1. Copy stable components (`EnergyStorage.h`, `Position.h`, `InventoryContainer.h`) to `core/include/gtnh/core/`
2. Implement serialization/deserialization for each component
3. Write `ComponentSerializationTest.cpp` with all 3 test cases
4. Update `simulation_core` to use `gtnh_core::EnergyStorage` instead of local version
5. Ensure no breaking changes to existing API

### Phase 3: PipeNetwork Core + BFS Tests (2 days)
1. Extract graph algorithms from `PipeNetwork.h/.cpp`:
   - Move `PipeNode`, `PipeEdge`, `PipeNetwork` structures
   - Move BFS implementation (`discoverNetwork`, `rebuildNetworks`)
   - Move `distributeEnergy` algorithm
2. Create `PipeNetworkGraph.h/.cpp` in `core/src/`
3. Write `PipeNetworkCoreTest.cpp` with all 4 test cases
4. Update `pipe_network` service to use `gtnh_core::PipeNetworkGraph`
5. Remove service-specific code from extracted files

### Phase 4: Schema Validation (1 day)
1. Create `SchemaValidator.h/.cpp` in `core/src/`
2. Implement validation for all .fbs files
3. Write `SchemaValidationTest.cpp` with all 4 test cases
4. Add validation to CI: `cmake --build && ctest`
5. Ensure validation runs as part of build process

## 6. Dependency Graph

```
gtnh_core
├── MachineRegistry
│   ├── data/registry/consumers.csv
│   └── data/registry/producers.csv
├── EnergyStorage (stable component)
├── Position (stable component)
├── InventoryContainer (stable component)
├── PipeNetworkGraph
│   ├── Graph algorithms
│   └── Energy distribution
└── SchemaValidator
    └── All .fbs files in protocol/

gtnh_core → simulation_core (depends on MachineRegistry, EnergyStorage, Position, InventoryContainer)
gtnh_core → pipe_network (depends on PipeNetworkGraph)
```

## 7. Risks and Open Questions

### 7.1 Migration Risks
1. **Circular Dependencies:** `MachineRegistry` may be needed by services that reference it
   - Mitigation: Use forward declarations and interface headers

2. **Memory Management:** Different allocators between old and new code
   - Mitigation: Use standard C++ smart pointers consistently

3. **Thread Safety:** Services may access registry from multiple threads
   - Mitigation: Add thread-safe accessors

### 7.2 Technical Risks
1. **Performance:** Extraction may introduce overhead
   - Mitigation: Profile and optimize critical paths

2. **ABI Compatibility:** Changes to exported APIs
   - Mitigation: Use version guards and maintain backward compatibility

3. **Build System:** Complex dependency management
   - Mitigation: Use modern CMake practices

### 7.3 Test Coverage
1. **Mocking:** Need to mock external dependencies
   - Mitigation: Use dependency injection patterns

2. **Integration Tests:** May need full service integration
   - Mitigation: Keep unit tests focused on core logic

## 8. Readiness Criteria

### 8.1 Technical Readiness
- [ ] All 4 phases implemented with specific file paths
- [ ] CMakeLists.txt for lib and tests complete
- [ ] All test cases from section 2 implemented
- [ ] Dependency graph verified
- [ ] No breaking changes to existing API

### 8.2 Build Readiness
- [ ] `cmake --build` succeeds without errors
- [ ] All tests pass (`ctest`)
- [ ] Schema validation runs as part of build
- [ ] Library can be installed and used by other projects

### 8.3 Quality Readiness
- [ ] Code follows C++20 best practices
- [ ] Memory safety verified
- [ ] Thread safety confirmed
- [ ] Performance benchmarks met

## 9. Current Issues Addressed

### 9.1 Hardcoded Machine Detection
**Problem:** `isMachineBlock()` uses hardcoded list: `block_id == 36 || block_id == 48 || block_id == 52 || block_id == 13 || block_id == 63`

**Solution:** Replace with `MachineRegistry::IsMachine(block_id)`

### 9.2 Hardcoded Slot Count
**Problem:** `defaultMachineSlotCount()` has hardcoded values for each block ID

**Solution:** Replace with `MachineRegistry::Get(block_id)->slots_in + slots_out`

### 9.3 No Protocol Tests
**Problem:** No tests for FlatBuffers schema validation

**Solution:** Add `SchemaValidationTest` with comprehensive schema checks

### 9.4 No Registry Tests
**Problem:** No tests for MachineRegistry functionality

**Solution:** Add `MachineRegistryTest` with CSV parsing and lookup tests

### 9.5 No PipeNetwork Tests
**Problem:** Limited tests for PipeNetwork core algorithms

**Solution:** Add `PipeNetworkCoreTest` with BFS and distribution tests

## 10. Build Commands

```bash
# Install Conan dependencies
conan install . --build=missing

# Build gtnh_core library
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make gtnh_core

# Build and run tests
make gtnh_core_test
./gtnh_core_test

# Run all tests via CTest
ctest

# Install gtnh_core for other projects
make install
```

## 11. Future Extensibility

The gtnh_core library is designed to be stable and extensible:

1. **Add new components:** Follow the same pattern for new stable components
2. **Extend tests:** Add new test cases for edge cases
3. **Update schemas:** SchemaValidator automatically validates new .fbs files
4. **Add algorithms:** New graph algorithms can be added to PipeNetworkGraph
5. **Version management:** Use semantic versioning for stable releases

This specification provides a complete roadmap for extracting stable core functionality from the GTNH Platform into a dedicated, test-driven library that can be maintained and evolved independently of the main project.