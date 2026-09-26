# Audit: machine coverage gaps (gp-2df)

**Date**: 2026-09-26
**Scope**: `src/game/machines/` — which systems still lack a unit test, and which of those are even reachable.
**Method**: `ctest --test-dir cmake-build-debug -N` for the real target list, then grep of the test
sources for the production symbol. No tests were written; no source was modified.

## Premise status: STALE — the issue's list is out of date

gp-2df was written before the machine test program landed. It claims these systems have
"no test referencing them":

> BoilerSystem, EBFSystem, LCRSystem, LargeBoilerSystem, ExplosionSystem, GeneratorSystem,
> RotareGeneratorSystem, SteamTurbineSystem, TransformerSystem

Verified against the tree, **8 of those 9 now have a dedicated registered ctest target**:

```
$ ctest --test-dir cmake-build-debug -N | grep -iE 'boiler|ebf|lcr|explosion|transformer|rotare|steam|machine'
  Test #10: explosion_system_test
  Test #11: transformer_system_test
  Test #12: steam_turbine_system_test
  Test #13: rotare_generator_system_test
  Test #14: ebf_system_test
  Test #15: lcr_system_test
  Test #16: boiler_system_test
  Test #17: machine_system_test
```

`src/game/machines/test/` contains 8 files, registered in `src/game/machines/CMakeLists.txt`
(lines 46, 101, 129, 156, 189, 220 and neighbours), each carrying the parent bead id in its comment
(`gp-g850` boiler, `gp-kxj5` rotare, `gp-q692` steam turbine, `gp-r3vd` EBF, `gp-mb2d` LCR).

The only genuinely untested system is **CoolantSystem** (gap 1 below). Everything else in the
original premise is now covered or is unreachable.

## Ranked gaps

### 1. CoolantSystem — REACHABLE and UNTESTED (highest real risk)

`src/game/machines/CoolantSystem.h:16-69` is a header-only `ISystem` implementing coolant-item
consumption. It is referenced **only by its own declaration** — it is never registered with the
simulation engine, so it never runs in production either:

```
$ grep -rn 'CoolantSystem' . --exclude-dir=cmake-build-debug --exclude-dir=cmake-build-release \
    --exclude-dir=.git --exclude-dir=worktrees
src/game/machines/CoolantSystem.h:16:class CoolantSystem : public ISystem {
src/game/machines/CoolantSystem.h:18:  explicit CoolantSystem(entt::registry &reg) : reg_(reg) {}
```

2 hits, both the declaration. Compare with a live system, which appears in `main.cpp`:
`src/apps/simcore/main.cpp:102-108` registers ExplosionSystem, GeneratorSystem, BoilerSystem,
TransformerSystem, RotareGeneratorSystem. `CoolantSystem` is absent.

**Consequence**: `HeatConstants::COOLANT_ITEM_ID` (`HeatConstants.h:12`) and
`COOLANT_COOLING_AMOUNT` (`:11`, = 50) have **zero** test references — they are the only two
constants in `HeatConstants.h` with no test coverage at all:

```
$ grep -rn --include=*.cpp --include=*.h -w COOLANT_COOLING_AMOUNT src | grep -v HeatConstants.h
src/game/machines/CoolantSystem.h:52      (sole use site)
$ grep -rn --include=*.cpp --include=*.h -w COOLANT_ITEM_ID src | grep -v HeatConstants.h
src/game/machines/CoolantSystem.h:35,43   (sole use sites)
```

**Risk assessment**: the *logic* is untested AND the *system* is dead. A test here would pin
unreachable code — see gap 2 for why that matters. The real finding is that `COOLANT_ITEM_ID =
ItemId::pack("0:11111:4")` is dead data: nothing consumes the coolant item, so the item exists in
the registry with no behaviour behind it. **Report, do not test.**

There is a latent bug worth noting even though it is unreachable: `CoolantSystem.h:42-50` scans
for a coolant slot and decrements, but the *first* loop at `:33-39` only checks `slot.item_id ==
COOLANT_ITEM_ID` without checking `count > 0`, so a slot with `item_id == COOLANT_ITEM_ID` and
`count == 0` sets `has_coolant = true`; the second loop then finds no `count > 0` slot, so the
machine is cooled for free. Unreachable today, so not ranked as a live risk.

### 2. RotareGeneratorSystem — UNREACHABLE (test pins dead logic)

The system **is** registered and **does** have a test (`rotare_generator_system_test`, test #13,
issued under gp-kxj5). But its only state-transition entry point has no production caller.

`RotareGeneratorSystem::activate()` at `src/game/machines/RotareGeneratorSystem.cpp:70-75` is the
sole writer of `RotareState` with `spinning = true`:

```cpp
void RotareGeneratorSystem::activate(entt::entity ent) {
    auto* state = reg_.try_get<RotareState>(ent);
    if (state && state->spinning) return;
    reg_.emplace_or_replace<RotareState>(ent, RotareState{true, kSpinDurationTicks, kEnergyPerTick});
}
```

`tick()` at `:13` bails immediately unless `state.spinning` (`:22`), and `RotareState` is
default-constructed with `spinning = false` (`RotareGeneratorSystem.h:16`). So with no caller of
`activate()`, `tick()` can never produce energy.

```
$ grep -rn '\->activate(\|\.activate(' src tools test 2>/dev/null
src/game/machines/test/test_rotare_generator_system.cpp:232:    f.sys.activate(ent);
   ... (26 further hits, ALL in that one test file)
```

**28 call sites, every one of them inside `test_rotare_generator_system.cpp`.** Zero in
production, zero in `tools/`, zero in `test/integration/`. This confirms and extends gp-18yv:
`RotareGeneratorSystem` is dead at runtime, and the rotare generator machine (block
`1110:100:1`, `machines.yaml:199-201`, `items.csv:410`) produces no EU for any player.

**Consequence for test value**: every assertion in `rotare_generator_system_test` about spin
rate, duration and energy-per-tick exercises a state machine that production cannot enter. Those
tests will keep passing if the feature is deleted, and will keep passing if the feature is
*wrong*. This is the gp-ajvg pattern (a passing test pinning unreachable logic). Not a gap to
close with more tests — a gap to close by wiring `activate()` or deleting the system.

The three constants involved (`RotareGeneratorSystem.h:32-35`: block id `ItemId::pack("1110:100:1")`
= 0xE801, `kSpinDurationTicks = 100`, `kEnergyPerTick = 32`) are all covered by the test but
describe unreachable behaviour.

### 3. GeneratorSystem — now covered (was the last real gap)

`GeneratorSystem::FuelValues()` (`GeneratorSystem.h:28`, delegating to
`content::generatorFuelValues()` at `GeneratorSystem.cpp:18-20`) and the burn/charge path
(`tick()` at `GeneratorSystem.cpp:32`) were the last machine system without a dedicated
`src/game/machines/test/` file at the time this audit started.

**Updated mid-session**: `src/game/machines/test/test_generator_system.cpp` landed while this
audit was in progress and is now registered as ctest target **#18 `generator_system_test`**, with
14 test bodies (lines 537-561) covering the declared capacity/max-output constants, zero and
negative capacity, recovery, and the steam gate. Covered.

### 4. MachineRegistry YAML load — covered

`MachineRegistry` is driven by `machines.yaml` and covered via
`test_HeatTransferSystem_yaml_generator_to_furnace` (`test_ecs_systems.cpp:434`) and
`test_recipe_manager_matching.cpp:1344-1347` (furnace/macerator/generator block ids). No gap.

## Constant-by-constant coverage of `HeatConstants.h`

All constants are listed with use sites and whether a test pins them. Values verbatim from
`src/game/machines/HeatConstants.h:6-17`.

| Constant | Value | Line | Production use | Test that pins it |
|---|---|---|---|---|
| `OVERHEAT_WARNING_THRESHOLD` | `0.90f` | :6 | `AdjacencyTransferSystem.cpp:132` | yes — `test_adjacency_transfer_system.cpp` |
| `OVERHEAT_CRITICAL_THRESHOLD` | `1.00f` | :7 | `AdjacencyTransferSystem.cpp:123` | yes — `test_adjacency_transfer_system.cpp:786` |
| `ENVIRONMENT_COOLING_RATE` | `4.0f` | :8 | `AdjacencyTransferSystem.cpp:190` | yes — `test_adjacency_transfer_system.cpp:243` |
| `WATER_COOLING_MULTIPLIER` | `3.0f` | :9 | `AdjacencyTransferSystem.cpp:191` | yes — `test_adjacency_transfer_system.cpp:245` |
| `EXPLOSION_DELAY_TICKS` | `60` | :10 | `ExplosionSystem.cpp:23` | yes — `test_explosion_system.cpp:103,191-235` |
| `COOLANT_COOLING_AMOUNT` | `50` | :11 | `CoolantSystem.h:52` (dead) | **NO** |
| `COOLANT_ITEM_ID` | `ItemId::pack("0:11111:4")` | :12 | `CoolantSystem.h:35,43` (dead) | **NO** |
| `CONVERSION_RATE` | `1` | :13 | `BoilerSystem.cpp:52,146,169` | yes — `test_boiler_system.cpp:194,344` |
| `HEAT_SINK_REPLENISH_TARGET` | `100` | :17 | `BoilerSystem.cpp:100,102,137,161` | yes — `test_boiler_system.cpp:195`, `test_boiler_ports.cpp:225` |

**One of nine constants in the shared heat header is untested**, and it is untested because the
only consumer is dead.

## Other numeric constants in `src/game/machines/`, coverage-checked

| Constant | Value | file:line | Test |
|---|---|---|---|
| `KANHAL_MAX_HEAT` | 1800 | `EBFSystem.h:35` | `test_ebf_system.cpp` (gp-r3vd) |
| `NICHROME_MAX_HEAT` | 2700 | `EBFSystem.h:36` | `test_ebf_system.cpp` |
| `TUNGSTENSTEEL_MAX_HEAT` | 4500 | `EBFSystem.h:37` | `test_ebf_system.cpp` |
| `COIL_LAYER_1/2`, `COIL_DX/DZ` | 1/2/1/1 | `EBFSystem.h:50-53` | `test_ebf_system.cpp` |
| `COAL_BLOCK_ID` | 1010 | `LargeBoilerSystem.h:27` | `test_boiler_system.cpp:198` |
| `CHARCOAL_BLOCK_ID` | 1011 | `LargeBoilerSystem.h:28` | `test_boiler_system.cpp:199` |
| `BOILER_HEAT_PER_FUEL` | 100 | `LargeBoilerSystem.h:29` | `test_boiler_system.cpp:196` |
| `STEAM_PER_WATER` | 10 | `LargeBoilerSystem.h:30` | `test_boiler_system.cpp:197` |
| `WATER_PER_TICK` | 1 | `LargeBoilerSystem.h:31` | `test_boiler_system.cpp` |
| `kForcePublishInterval` | 10 | `MachineSystem.h:54` | `test_machine_system.cpp` (gp-3700888b) |
| `kSteamFillQuantum` | 1000 | `MachineSystem.h:60` | `test_machine_system.cpp` |
| `kSpinDurationTicks` / `kEnergyPerTick` | 100 / 32 | `RotareGeneratorSystem.h:34-35` | tested but **unreachable** |
| `kBlockId` (steam turbine) | `ItemId::pack("1110:010:44")` = 0xE42C | `SteamTurbineSystem.h:27` | `test_steam_turbine_system.cpp` |
| `BATTERY_LV/MV/HV` | 60948/60949/60950 | `ItemEnergyStorage.h:30-32` | mining tests |
| `kMaxOutputSize` | 64 | `DrillComponent.h:62` | `test_mining_systems.cpp` |

Note: `LargeBoilerSystem` is *not* an untested system — `test_boiler_system.cpp` covers it
throughout (fixture at :258, 15+ test bodies from :568 to :941). gp-2df listed it as untested;
that is no longer true.

## Summary

- **1 real gap**: `CoolantSystem` — the only untested system, but it is itself unregistered, so
  the gap is "dead feature", not "untested live feature". Report; do not write a test.
- **1 confirmation + escalation of gp-18yv**: `RotareGeneratorSystem::activate()` has 28 call
  sites, all in its own test file. The system cannot run in production. Its test file pins
  unreachable logic.
- **Premises that are FALSE**: the issue's claim that 9 systems have no test (8 of 9 now have
  dedicated ctest targets, and the 9th — `GeneratorSystem` — got one mid-audit as target #18),
  and the implicit claim that `LargeBoilerSystem` is untested (`test_boiler_system.cpp` covers
  it extensively).

Re-verified at HEAD `db4f2060`, after the worktree advanced mid-audit: all 4 `block_id: "0"`
rows, all 9 `HeatConstants` coverage verdicts, and the `activate()` call-site count are unchanged.

## Re-verification note

This audit ran against a worktree that moved twice during the session (`fe23542c`, then
`db4f2060`), adding `test_generator_system.cpp` and a `chemical_reactor_lv` row to
`machines.yaml`. Every claim above was re-checked at the final HEAD. The one claim that changed
is documented in gap 3.
