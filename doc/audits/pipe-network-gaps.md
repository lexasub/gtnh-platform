# Audit: pipe_network graph-algorithm gaps (gp-lbs)

**Date**: 2026-09-26
**Scope**: `src/apps/pipe_network/` graph and flow code versus `pipe_network_test.cpp`
(142 KB, **112** `test_` functions, registered as ctest target #41 `pipe_network_test`).
**Method**: read the algorithm bodies, then grep the test source for each shape the issue asks
about. No tests were written; no source was modified.

## Premise status: PARTIALLY STALE

gp-lbs names six shapes: cycles, disconnected components, a node with no consumers, a cycle with
no producer, concurrent modification during solve, and flow direction. Status of each:

| Shape gp-lbs asks about | Status |
|---|---|
| disconnected components | **covered** — `test_disconnected_graphs` (`:81`), asserted `:91,:111` |
| node with no consumers | **covered** — `test_item_network_no_sink` (`:530`), `test_energy_distribution_no_sink` (`:665`) |
| concurrent modification during solve | **not a code bug** — see gap 5 |
| cycles | **not tested, but the code is provably cycle-safe** — see gap 4 |
| cycle with no producer | **not tested, subsumed by cycles** — see gap 4 |
| flow direction | **not tested, and the code has a real asymmetry** — see gap 3 |

So the two shapes most likely to hide a bug (cycles) are safe, and the one with a real defect
(flow direction) is not on the issue's list in that form. The genuinely under-covered area is
**reachability**, not topology.

## Ranked gaps

### 1. `distributeEnergy` and `distributeFluid` are TEST-ONLY code (highest impact)

This is the biggest finding and it is not a missing test — it is code that exists, is documented,
is tested, and **never runs**.

```
$ grep -rn 'distributeEnergy' --exclude-dir=cmake-build-debug --exclude-dir=cmake-build-release \
    --exclude-dir=worktrees --exclude-dir=.git --exclude-dir=graphify-out .
src/apps/pipe_network/PipeNetworkService.cpp:1465:  // ... never in the EU-domain roles consumed by distributeEnergy() — the legacy
src/apps/pipe_network/PipeNetwork.cpp:518:std::unordered_map<uint64_t, int32_t> PipeNetworkManager::distributeEnergy(
src/apps/pipe_network/PipeNetwork.h:232:  std::unordered_map<uint64_t, int32_t> distributeEnergy(
src/apps/pipe_network/pipe_network_test.cpp:657,677,699:  (three call sites)
... everything else is doc/, .beads/, openspec/, graphify-out/
```

Zero production callers. The service's only per-tick distribution call is
`PipeNetworkService.cpp:253`:

```cpp
network_manager_.distributeHeat(net->id, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
```

`distributeFluid` is the same story — `grep -rnw distributeFluid src | grep -v _test` yields only
its own definition and declaration at `PipeNetwork.cpp:654` / `PipeNetwork.h:236`, plus a comment.

**Consequence**: every EU/fluid test in the suite is testing a code path that no tick reaches.
`test_energy_distribution_simple` (`:630`), `_no_sink` (`:665`), `_capacity_limited` (`:683`),
`test_fluid_distribution_simple` (`:739`), `_capacity_limited` (`:838`), `_no_source` (`:861`) —
6 tests, ~180 lines, all green, all unreachable. That is the gp-ajvg pattern in its purest form.

The code itself carries a hint of this. `distributeFluid` at `:658-659` says "Legacy distribution
remains available for existing callers" — but there are no existing callers left.

**Recommendation**: this is a bead of its own ("delete or wire distributeEnergy/distributeFluid"),
higher priority than any missing test. Do not add tests to unreachable code.

### 2. The returned `deltas` map is untested for EU and fluid (would catch a real double-count)

`distributeFlow` (`:453-515`) both **mutates the node buffers directly** and **accumulates the same
amounts into `deltas`**:

```cpp
deltas[sid] -= take;
ni->second.energyBuffer -= take;     // :489-490
...
deltas[snid] += give;
ni->second.energyBuffer += give;     // :509-510
```

`distributeHeat` (`:1234`) does exactly the same (`:1338-1339`):

```cpp
sourceNode.heatStored -= take;
node.heatStored += take;
deltas[sourceId] -= take;
deltas[sinkId] += take;
```

So `deltas` is a *mirror* of a mutation that has already happened. Any caller that applied the map
to the same buffers would double-apply. No caller does today (gap 1), which is why it is safe —
but the API invites the bug, and it is the exact kind of invariant a test should pin.

The test suite checks `deltas` for **heat** but not for EU/fluid:

```
$ grep -n 'deltas' src/apps/pipe_network/pipe_network_test.cpp
657:  auto deltas = mgr.distributeEnergy(targetNetId, 300);
658:  CHECK(!deltas.empty(), "energy distribution produced deltas");     ← non-emptiness only
759:  auto deltas = mgr.distributeFluid(targetNet, 200);
760:  CHECK(!deltas.empty(), "fluid distribution produced deltas");      ← non-emptiness only
798:  CHECK_EQ(deltas[pipe], 250, ...)                                   ← heat: exact values
799:  CHECK_EQ(deltas[boiler], -250, "source mirror is debited exactly once")
1164,1203,1207,1271,1272,1284,1285:  heat — exact values
```

Only `distributeHeat` gets `CHECK_EQ(deltas[x], exact)`. EU and fluid get `!deltas.empty()`.
The invariant that would be worth pinning is: **the sum of positive deltas equals the sum of
negative deltas, and equals the amount actually moved in the buffers** — i.e. the map mirrors the
mutation exactly, with no double-count. `:798-799` already demonstrates the pattern for heat and
is the model to copy.

This is the best remaining test-shaped gap, and it is low-cost because the fixture pattern is
already there.

### 3. Flow direction is asymmetric between the two distribution paths (real behaviour difference, untested)

`distributeFlow` (EU) is **source-pull**: it debits sources first (`:481-495`), then credits sinks
from `totalAmount` (`:497-515`) — and the sink credit is computed from the *original* `totalAmount`,
not from what the sources actually produced.

```cpp
if (!sinks.empty()) {
    int32_t perSink = totalAmount / static_cast<int32_t>(sinks.size());   // :498
```

`distributeFluid` is **sink-push only**: it never debits a source at all (`:668-698`). It computes
`perSink` from `tickFluid` and pushes. `distributeHeat` is also sink-push, but it does debit
sources (`:1324` `sourceNode.heatStored -= take`).

So the three paths differ in a way that is invisible from outside:

| path | debits sources? | credits computed from |
|---|---|---|
| `distributeFlow` (EU) | yes, `min(take, energyBuffer)` per source (`:487`) | `totalAmount` (the *requested* amount) |
| `distributeFluid` | **no** | `tickFluid` |
| `distributeHeat` | yes, but only `heatStored - 0.9*capacity` excess (`:1328`) | actual excess accumulated at `:1275-1281` |

The EU path is the interesting one: if the sources cannot supply `totalAmount` (buffers nearly
empty, or `domain.rate` clamps at `:491-493`), the sinks still receive a full `totalAmount` split.
Energy is **created**. Concretely: one source with `energyBuffer=10, energyCapacity=1000`, asked to
move `totalAmount=100`, gives 10 (`:487` clamps to `energyBuffer`), but a sink with
`energyCapacity=2000` receives 100 (`:499-501` clamps only to *room*, not to what was taken).
90 EU appear from nothing (measured below).

Verified by replaying the `distributeFlow` arithmetic (`:453-515`) with a single source
`buffer=10, capacity=1000, rate=0` and a single sink `buffer=0, capacity=2000, rate=0`, asked for
`totalAmount=100`:

```
source buffer 10 -> 0     (gave 10, clamped by energyBuffer at :487)
sink   buffer  0 -> 100   (gave 100, clamped only by "room" at :501)
net EU created = 90
```

Control case with a well-stocked source (`buffer=10000`, which is what the existing test uses):
source gives 100, sink gets 100, balanced. So the defect only appears when sources are short —
precisely the case the suite never constructs.

The sink path is untested for this. `test_energy_distribution_capacity_limited` (`:683`) sets
`energyBuffer=10000` on the source — i.e. it deliberately avoids the shortfall case — and
`_no_sink` (`:665`) only checks that the source drained, with a comment at `:678` explicitly
calling the drain-without-sink behaviour "existing behavior".

**This is a real, reachable, untested failure mode** — but note it is only reachable via
`distributeEnergy`, which per gap 1 has no production caller. So: fix gap 1 first (wire or delete),
then this becomes a live bug the moment it is wired. Ranked below the dead-code finding because
its blast radius is currently zero, and above everything else because it is the only place I found
where the algorithm is wrong rather than merely untested.

### 4. Cycles and cycles-with-no-producer: NOT TESTED, but provably safe — do not write these tests

gp-lbs asks for cycle coverage. There is no cycle test:

```
$ grep -niE 'cycle|cyclic|loop|ring|square' src/apps/pipe_network/pipe_network_test.cpp
1649:  // 3.5.3/3.5.4: pending lifecycle ...
2170:  // 2.6.3: owner id 0 is a valid machine instance ...
2932:static void test_persistence_load_unload_cycle() {
3175:  // Persistence cycle
```

Every hit is a comment or the word "cycle" in "lifecycle"/"unload_cycle". No test builds a ring.

However, **all four traversals in this subsystem maintain a `visited` set**, so cycles terminate
by construction:

- `PipeNetworkManager::bfsNetwork` — `visited.insert(startNode)` at `:351`, and
  `if (visited.find(neighbor) == visited.end())` at `:369`
- `PipeNetworkManager::discoverNetwork` — `:382`, `:400`
- `CableGraph::tick`'s untargeted broadcast BFS — `:174-177`, `:224-225`
- `CableGraph::findPath` — `:365-369`, `:409-410`
- `moveItemsInNetwork`'s sink search — `:983-986`, `:1002`

I traced each. There is no recursion anywhere in the graph code (all iterative, `std::queue`),
and every neighbour enqueue is guarded by a membership test in `visited`. A cycle cannot
infinite-loop, and a cycle with no producer simply yields an empty path, which every caller
already handles with `if (path.empty()) continue;` (`CableGraph.cpp:237-239`).

**Verdict: theoretical gap on a correct algorithm.** gp-lbs' own standard — "a theoretical gap on
a correct algorithm is not worth a test" — says skip. Stated explicitly so it is not re-raised.

The one nuance worth a cheap assertion if a cycle test is ever written for another reason: BFS
returns *a* shortest path, and in a cycle the first path found depends on the iteration order of
`adjacency`, which is built from `edges_` (an unordered container) at `:355-358` and `:943-946`.
So on a cycle, the chosen route is **not deterministic across runs**. `findPath` at `:361` has
this property, and `moveItemsInNetwork` at `:990-1000` too. If any future test asserts a specific
path through a cycle it will flake. That is a reason *not* to write the test, and a good reason to
note it.

### 5. Concurrent modification during solve: not a code bug, but the invariant is untested

There is no reentrancy hazard in the current shape. Each `distribute*` walks
`net.nodeIds` (a `std::vector` snapshot taken by `rebuildNetworks`, `:409`) and looks nodes up with
`nodes_.find(...)` every iteration, re-checking `end()` — e.g. `:461`, `:483`, `:500`. Removal
during iteration would be handled, not crashed on.

The genuinely untested part is narrower: `rebuildNetworks` invalidates `nodeToNetwork_` and
`networks_` (`:409-410`), and there is no test that calls `addNode`/`removeNode`/`addEdge` *while*
holding a reference obtained from a prior `getAllNetworks()` call. `test_remove_edge_and_rebuild`
(`:1294`) and `test_add_edges_rebuilds_once_and_deduplicates` (`:1312`) cover the sequential
rebuild contract, and `test_node_removal_cancels_pending_late_response_dropped` (`:1725`) covers
removal during a pending-transaction lifecycle — but not a raw reference-invalidation case.

**Verdict: theoretical, low value.** Listed so the shape is not mistaken for an untested bug.
If a test is ever added, the cheap form is: hold `const auto* net` from `getAllNetworks()`, call
`removeNode` on one of its members, then use `net` — assert no crash and that `rebuildNetworks`
produced a fresh graph. That is a hardening test, not a defect fix.

### 6. `CableGraph::processOverheat` mutates `m_nodes` after iterating it (correct, but subtle)

`:281-304`:

```cpp
for (auto& pair : m_nodes) {            // :282  — pass 1, mutates temperature
    ...
    if (result.exploded) m_explodedThisTick.push_back(...);   // :297
}
for (const auto& exploded : m_explodedThisTick) {
    removeCableNode(exploded.nodeId);   // :302  — pass 2, erases from m_nodes
}
```

The erase happens in a **second loop** over `m_explodedThisTick`, not while iterating `m_nodes`, so
this is correct and is not UB. Good design, actually.

But `m_explodedThisTick` is **not cleared inside `processOverheat`** — only at the top of `tick()`
(`:141`). `processOverheat` is private (`:79`) and called only from `tick()` (`:278`), so today
there is no path that double-accumulates. If `processOverheat` is ever called from anywhere else,
the second call would re-remove already-removed nodes and accumulate duplicates in
`m_explodedThisTick`, which is the vector `getExplodedNodes()` (`:61-63`) hands out by const
reference to the service. No test pins the "cleared exactly once per tick" invariant, and
`test_cable_graph_overheat_explosion` (`:1036`) and `_ampacity_overheat` (`:1072`) check the
explosion fires, not that the vector is per-tick.

**Verdict: theoretical today (private, single caller), but the cheapest defensive test in the
file** — one tick, assert `getExplodedNodes().size()`, tick again, assert it did not grow.

## Summary — ranked by real risk

| # | Gap | Reachable? | Code actually wrong? | Worth a test? |
|---|---|---|---|---|
| 1 | `distributeEnergy`/`distributeFluid` have zero production callers; 6 tests cover dead code | n/a (dead) | n/a | **No — delete or wire first** |
| 2 | `deltas` map mirrors a mutation; only heat pins exact values | via test only | latent double-count if wired | **Yes — highest-value test** |
| 3 | `distributeFlow` credits sinks from requested `totalAmount`, not actual supply → EU created | zero today (gap 1) | **yes** | after gap 1 is resolved |
| 4 | Cycles / cycle-with-no-producer | yes | no — `visited` in all 5 traversals | **No — correct algorithm** |
| 5 | Reference invalidation during solve | yes | no — re-checks `end()` each iteration | No — hardening only |
| 6 | `m_explodedThisTick` cleared only in `tick()`, not `processOverheat()` | yes | no — private, 1 caller | Cheap, optional |

Item 3 is the only genuine defect found, and it is currently latent behind item 1. Resolving item 1
is the prerequisite for everything else in this file: **the pipe_network test suite is 6 tests
wider than the production code path it appears to cover.**
