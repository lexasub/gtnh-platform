# Audit gp-tha — `doc/c4/` diagrams vs the actual service topology

**Date:** 2026-09-26 · **Commit:** `926521cc` · **Scope:** read-only. No diagram is modified.

`AGENTS.md:141` declares `doc/c4/` the "Authoritative C4 diagrams" and `AGENTS.md:2`
points at `doc/c4/README.md`. This audit tests that claim by tracing the **actual**
router `Subscribe`/`Publish` calls and TCP listeners, not the diagram arrows.

Method: for each service I read its `main()` and router client to get the real topic
list and ports, then compared against the `.puml` source (the PNGs are generated from
these, so a `.puml` finding is a PNG finding).

**Verdict: `doc/c4/` is NOT currently authoritative.** 21 disagreements, 4 of them
structural (a service is drawn as a peer that has no process, or a wire that does not
exist).

---

## 1. Structural disagreements — the worst ones

### C4-01 (CRITICAL) — `:5555` does not exist. RecipeManager is a pub/sub service, not a TCP RPC service.

The single most-repeated falsehood in the diagram set. Every occurrence:

| File:line | Text |
|---|---|
| `doc/c4/level2-container.puml:49` | `Container(recipeman, "RecipeManager", "C++\n:5555 (router)", ...)` |
| `doc/c4/level4-deployment.puml:23` | `' :5555 — RecipeManager (TCP RPC, router-registered)` |
| `doc/c4/level4-deployment.puml:46` | `Container(recipeman_d, "reciped", "C++ :5555 (router)", ...)` |
| `doc/c4/README.md:115` | `…:5555 recipe_manager…` |
| `doc/c4/level2-container.puml:85` (by implication) | `Rel(router, recipeman, "recipe.* / machine.*", $tags="tag_pubsub")` — the arrow is right, the port is invented |

The code:

```
$ grep -rn "5555" --include=*.cpp --include=*.h --include=*.go src/ tools/
(no hits)
```

`reciped` opens **no listening socket**. It builds `gtnh::pipenet::MessageRouterClient`,
sets a service name, and subscribes to topics:

- `src/apps/recipe_manager/main.cpp:42-43,48-50` — `routerHost`/`routerPort` (default 4000), `router.SetServiceName("recipe_manager")`
- `src/apps/recipe_manager/RecipeManagerService.cpp:27-33` — subscribes `recipe.check`, `recipe.craft`, `recipe.evaluate`, `recipe.catalog`, `recipe.item`, `recipe.machine`, `world.block_entity.update`
- `RecipeManagerService.cpp:90,170` — publishes to reply topics and `recipe.completed`

The correct description is "C++ / router-registered pub/sub, no listener" — which is
exactly what `level4-deployment.puml:46`'s own parenthetical "(:5555 (router))"
half-admits before asserting a port that no process binds.

The `:5555` fiction also propagates into `ROADMAP.md:274,635,664` and
`README.md:73,85,101`.

### C4-02 (CRITICAL) — SpatialIndex is drawn as a live peer service with R-tree queries; it is a 2-line stub that is not built

- `doc/c4/level2-container.puml:40` — `Container(spatial, "SpatialIndex", "C++ (Boost.Geometry R-tree)", "Пространственный индекс\nпоиск мультиблоков\nO(log n) AABB queries\nsrc/apps/spatial_index", $tags="planned")`
- `doc/c4/level2-container.puml:120` — `Rel(spatial, chunkstore, "R-tree queries:\nGetMultiblocksIntersecting", $tags="tag_planned")`
- `doc/c4/level3-tiny-services.puml:26-28` — a `Container_Boundary` with `main.cpp` ("2-строчный stub") **and** a live `SpatialIndex stub` component promising "R-tree (Boost.Geometry), Octree"
- `doc/c4/level4-deployment.puml:47` — `PLAN_CONTAINER(spatial_d, "spatiald", "C++", "SpatialIndex\n(PLANNED)")` — and note the process name `spatiald` matches **no** target; the real one is `spatialindexd` (`src/apps/spatial_index/CMakeLists.txt:1`)

The code:

```
$ cat src/apps/spatial_index/main.cpp
1  // Spatial index stub
2  int main() { return 0; }
$ grep -n "add_subdirectory" CMakeLists.txt
84:#add_subdirectory(src/apps/spatial_index)
```

The `$tags="planned"` / `PLAN_CONTAINER` treatment is correct in spirit, but the
component descriptions ("O(log n) AABB queries", "R-tree (Boost.Geometry)",
"GetMultiblocksIntersecting") describe a **function that does not exist** and are written
in the present tense. `AGENTS.md:141` and `AGENTS.md:194` get this right ("STUB — not
implemented, not built"), so the authoritative-doc and the diagram contradict each other
inside the same repo. The diagram is wrong.

Also: `level3-tiny-services.puml:27` names the Boost.Geometry R-tree as the plan, but
`src/apps/spatial_index/CMakeLists.txt:5` has `#boost::boost` **commented out** and
Boost is not in `conanfile.txt` at all — so the diagram describes a dependency the plan
cannot currently satisfy.

### C4-03 (CRITICAL) — WorldGenerator is drawn as a networked service that talks to ChunkStore; it is a library statically linked into ChunkStore

- `doc/c4/level2-container.puml:33` — `Container(worldgen, "WorldGenerator", "C++ (FastNoiseLite)", "Генерация чанков\nпроцедурный террейн\nбиомы\nsrc/apps/world_generator")` — drawn as a Container **inside the server system boundary** alongside the daemons
- `doc/c4/level2-container.puml:89-90` — `Rel(router, worldgen, "world.chunks.generate", $tags="tag_pubsub")` and `Rel(worldgen, router, "world.chunks.generated", $tags="tag_pubsub")` — **two pub/sub edges**
- `doc/c4/level2-container.puml:103` — `Rel(worldgen, chunkstore, "Генерирует чанки → AsyncSetBlock\nподключается через gen_queue_", $tags="tag_rpc")` — a third edge
- `doc/c4/level4-deployment.puml:44` — `Container(worldgen_d, "worldgend", "C++", "WorldGenerator\n(запускается после chunkd)")` — a **process** in the startup-order graph, with dependencies at `:60` and an edge at `:75`

The code — there is no WorldGenerator process and no WorldGenerator router client:

```
$ ls src/apps/world_generator/
AGENTS.md  CMakeLists.txt  GenerationQueue.cpp  GenerationQueue.h  OreConfig.cpp
OreConfig.h  OreGenerator.cpp  OreGenerator.h  OreTypes.h  README.md  SurfaceHeights.cpp
SurfaceHeights.h  test  TreeGenerator.cpp  TreeGenerator.h  WorldGenerator.cpp
WorldGenerator.h
(no main.cpp)
$ grep -rn "worldgen\|WorldGenerator" src/apps/chunk_store/CMakeLists.txt
63:    worldgeneratord # TODO - may be remove deps
103,125,140,155,170:  worldgeneratord
```

`src/apps/world_generator/CMakeLists.txt:7` is `add_library(worldgeneratord ...)` — a
**static library**, linked into `chunkd`. It cannot have its own router connection, its
own listener, or its own position in a startup order.

`AGENTS.md:52` gets this right ("library, no binary") and so does
`doc/c4/README.md:113` ("**WorldGenerator**: библиотека (нет main.cpp)"). So
`doc/c4/README.md` and `doc/c4/level2-container.puml` contradict each other **inside
`doc/c4/` itself**.

### C4-04 (CRITICAL) — Validation is drawn as a static library; it is an unbuilt *executable*, and the diagram gives SimCore and RecipeManager RPC edges to it

- `doc/c4/level2-container.puml:56` — `Container(validation, "Validation Library", "C++ (static lib)", ...)`
- `doc/c4/level2-container.puml:100` — `Rel(simcore, validation, "Валидация инвентаря\nперед сохранением", $tags="tag_rpc")`
- `doc/c4/level2-container.puml:114` — `Rel(recipeman, validation, "Валидирует входные\nItemStack для крафта", $tags="tag_rpc")`
- `doc/c4/level3-tiny-services.puml:34-41` — `Container_Boundary(val, "Validation Library (C++ static lib)")` containing both the validators **and** `Component(val_main, "main.cpp + src/apps/validation", "C++", "Точка входа для тестов")` — a `main()` inside something labelled a static library

The code:

```
$ cat src/apps/validation/CMakeLists.txt
5  add_executable(validationd
6      main.cpp
7      src/validation.cpp
8  )
$ grep -n "validation" CMakeLists.txt
(no add_subdirectory line at all — not even commented)
```

It is an **executable target** (`validationd`), and it is **never added to the build** —
unlike SpatialIndex, which at least has a commented `add_subdirectory` at
`CMakeLists.txt:84`. So the two "RPC edges" at `level2-container.puml:100,114` connect
to a binary that cannot be produced, let alone called. `AGENTS.md:58` says "not in
default build" — correct, and it is stronger than the diagram implies.

---

## 2. Topology: the real router edges vs the drawn ones

I traced every `Subscribe(...)` / `Publish(...)` in the tree. Here is the real map,
then the deltas.

### Actual per-service topics (verified by grep, with file:line)

| Service | Subscribes to | Source |
|---|---|---|
| `chunkd` | `chunk.requests` | `src/apps/chunk_store/Network/IoUringRouterClient.cpp:25` |
| `simcored` | `player.actions`, `player.actions.setblock`, `world.blocks.changed`, `fluid.consume.response`, `energy.consume.response`, `item.flow`, `item.transfer.response`, `player.chest.open`, `player.chest.close`, `player.machine.open`, `player.machine.close`, `player.gamemode.change`, `player.scenario.start`, `player.inventory.load`, `meta_db.quest.get.response`, `quest.complete.request`, `quest.book.open` | `src/apps/simcore/main.cpp:576-592` |
| `reciped` | `recipe.check`, `recipe.craft`, `recipe.evaluate`, `recipe.catalog`, `recipe.item`, `recipe.machine`, `world.block_entity.update` | `src/apps/recipe_manager/RecipeManagerService.cpp:27-33` |
| `pipe_networkd` | `energy.node.update`, `energy.check.request`, `energy.consume.request`, `fluid.node.update`, `fluid.check.request`, `fluid.consume.request`, `item.node.update`, `item.transfer.request`, `world.blocks.changed`, `world.chunk.loaded.compressed`, `world.machine.config.updated`, `pipe.wrench.action`, `pipe.contents.request` | `src/apps/pipe_network/PipeNetworkService.cpp:139-151` |
| `entitystated` | `entity.state.get`, `entity.state.set`, `world.blocks.changed` | `src/apps/entity_state_store/main.cpp:238-240` |
| `metadbd` | `meta_db.inventory.get`, `meta_db.inventory.set`, `meta_db.inventory.snapshot`, `meta_db.quest.get`, `meta_db.quest.set`, `quest.completed`, `quest.exchange.request`, `quest.exchange.cooldown.get`, `player.joined`, `player.left` | `src/apps/meta_db/router_client.go:103-114` |
| `gatewayd` | 26 topics: `metadb.player.online`, `world.chunk.loaded.compressed`, `world.blocks.changed`, `entities.#`, `sim.multiblock.created/destroyed`, `player.actions.ack/directive`, `player.inventory.update`, `sim.craft.response`, `sim.workbench.state/load`, `player.machine.slot.response`, `player.tool.action.response`, `world.block_entity.update`, `recipe.completed`, `player.position.load`, `quest.completed.notification`, `quest.unlocked`, `quest.progress.updated`, `quest.era.transition`, `meta_db.quest.get.response`, `quest.exchange.response`, `quest.exchange.cooldown.response`, `player.gamemode.changed` | `src/apps/gateway/main.cpp:114-139` |

### C4-05 (HIGH) — the diagram's ChunkStore topics do not exist

- `doc/c4/level2-container.puml:79-80` — `Rel(router, chunkstore, "world.blocks.*\nworld.chunks.*")` and `Rel(chunkstore, router, "world.blocks.changed\nworld.chunks.saved")`
- `doc/c4/level3-chunkstore.puml:65` — `Component(router_client, "RouterClient", "C++", "TCP клиент MessageRouter\npub/sub: world.blocks.changed\nworld.chunks.saved")`

The code: `src/apps/chunk_store/Network/IoUringRouterClient.cpp:25` subscribes **only**
to `chunk.requests`, and `:41` rejects anything that is not `chunk.requests`
(`if (topic != "chunk.requests" ...) return;`). It publishes exactly one topic —
`world.chunk.loaded.compressed` (`:73`).

So **all four drawn ChunkStore topic names are wrong**: `world.blocks.*` (not a topic),
`world.chunks.*` (not a topic), `world.blocks.changed` (SimCore and entitystated and
pipenet subscribe to it; **chunkd never publishes it**), `world.chunks.saved`
(zero occurrences anywhere in `src/`).

### C4-06 (HIGH) — the diagram's MetaDB inbound topics are a subset; the outbound set is wrong

- `doc/c4/level3-meta-db.puml:16` — lists `meta_db.inventory.get/set/snapshot, meta_db.quest.get/set, quest.completed, quest.exchange.request/cooldown.get, player.joined, player.left` — this is **exactly right**, verified against `router_client.go:103-114`. Credit where due.
- `doc/c4/level2-container.puml:83` — `Rel(router, metadb, "meta_db.inventory/quest.*\nquest.* / player.joined/left")` — accurate summary.
- ❌ `doc/c4/level2-container.puml:84` — `Rel(metadb, router, "player.inventory.load\nplayer.position.load")` — accurate but **incomplete**. MetaDB also publishes `quest.progress.updated` (`quest_handlers.go:130`), `quest.exchange.response` (`exchange_handlers.go:44`), `quest.exchange.cooldown.response` (`exchange_handlers.go:238`), `metadb.player.online` (`router_client.go:117`), and arbitrary `handlerTopic` replies (`quest_handlers.go:62`).

### C4-07 (MEDIUM) — the diagram's SimCore `multiblock.*` topics use the wrong prefix

- `doc/c4/level2-container.puml:82` — `Rel(simcore, router, "sim.*\nmultiblock.*", $tags="tag_pubsub")`

The real published names are `sim.multiblock.created` and `sim.multiblock.destroyed`
(`src/apps/gateway/main.cpp:118-119`). There is no bare `multiblock.*` topic — and
notably these are **published by simcore but consumed by gateway**, which the diagram
gets directionally right but names wrong.

### C4-08 (MEDIUM) — PipeNetwork is drawn with one opaque `pipe_network.*` topic; it has 13

- `doc/c4/level2-container.puml:87` — `Rel(router, pipenet, "pipe_network.*", $tags="tag_pubsub")`
- `doc/c4/level4-deployment.puml:89` — PrioLow list includes `pipe_network.*`

Zero occurrences of `pipe_network.` as a topic prefix in `src/`. The real surface is
`energy.*`, `fluid.*`, `item.*`, `pipe.*`, `world.*` — 13 subscriptions
(`PipeNetworkService.cpp:139-151`) and 6+ publications (`:273,320,334,792,856,961,993`),
including `item.flow`, `energy.flow`, `energy.cable.exploded`, `pipe.wrench.response`,
`pipe.contents.response`, `fluid.pipe.state`.

### C4-09 (MEDIUM) — the diagram omits the SimCore → PipeNetwork "direct RPC" that `README.md:84` also claims

- `doc/c4/level2-container.puml:98` — `Rel(simcore, pipenet, "RPC: energy/fluid tick\ndistributeEnergy", $tags="tag_rpc")`

The real coupling is **pub/sub**, not TCP RPC. `PipeEnergyClient` holds a
`router_` and publishes (`src/apps/simcore/Network/PipeEnergyClient.cpp:46,80,106`:
`energy.node.update`, `energy.consume.request`). There is no TCP connection from simcore
to pipenet. Same for `README.md:84`.

### C4-10 (MEDIUM) — the gateway's 26 subscriptions are invisible in the diagram; only 2 are represented

- `doc/c4/level2-container.puml:75-76` — `Rel(gateway, router, "TCP pub/sub\nRegister / Subscribe / Publish")` and `Rel(router, gateway, "world.* / player.*\nrouter → gateway relay")`

`world.* / player.*` describes 6 of 26. The whole quest subsystem
(`quest.completed.notification`, `quest.unlocked`, `quest.progress.updated`,
`quest.era.transition`, `quest.exchange.response`, `quest.exchange.cooldown.response`),
the gamemode topics, the machine-slot/tool-action responses, and the workbench state
topics are absent. `level3-gateway.puml` does not show the topic layer at all — it only
shows the two TCP servers (`:24-25`, which **are** correct: ctrl `:7777`, bulk `:7778`).

### C4-11 (MEDIUM) — the `level4-deployment.puml` topic-priority map is wrong on all three tiers

`doc/c4/level4-deployment.puml:87-89` assigns topics to PrioHigh / PrioNormal / PrioLow.
The real classifier is `src/apps/message_router/router.go:62-74`:

| Tier | Diagram claims (`:87-89`) | Actual `classifyTopic` (`router.go:64-73`) |
|---|---|---|
| PrioHigh | `player.actions`, `player.actions.ack` | `player.actions.ack`, `player.actions` — ✅ **correct** |
| PrioNormal | `world.blocks.changed`, `world.chunks.*`, `player.inventory.update` | `world.*` (prefix), `player.inventory.update`, `player.inventory.load`, `player.joined`, `meta_db.inventory.set`, `meta_db.inventory.get`, `metadb.player.online` — **4 of 7 missing** |
| PrioLow | `entities.*`, `simulation.*`, `sim.*`, `multiblock.*`, `recipe.*`, `machine.*`, `entity.state.*`, `meta_db.*`, `pipe_network.*` | everything else, by `default:` — the list is unenumerable, and `meta_db.*` is **wrong** (it is PrioNormal) |

Two concrete errors: `player.inventory.load`, `player.joined`, `metadb.player.online` are
PrioNormal and the diagram files them under PrioLow or omits them; and `meta_db.*` is
PrioNormal (`router.go:70-71`) but the diagram lists it in PrioLow (`:89`).

### C4-12 (LOW) — `entity.state.*` is drawn as the EntityStateStore topic family; two of the three names are wrong

`doc/c4/level2-container.puml:88` — `Rel(router, ess, "entity.state.*", $tags="tag_pubsub")`.
Actual (`src/apps/entity_state_store/main.cpp:238-240`): `entity.state.get`,
`entity.state.set` (subscribed), `entity.state.response` (`:204,211`), `entity.state.ack`
(`:229`), plus `world.blocks.changed`. So the prefix is right but the family is larger,
and `world.blocks.changed` is an undocumented second subscription.

---

## 3. Ports

Every port claim in the diagram set, checked against the code:

| Port | Diagram | Code | Verdict |
|---|---|---|---|
| 4000 router | `level4-deployment.puml:18,36,67-69,72-74` | `src/apps/message_router/main.go:23` | ✅ |
| 5001 chunkd | `level4-deployment.puml:19` | `src/apps/chunk_store/main.cpp:29` | ✅ |
| 5005 MetaDB JSON | `level4-deployment.puml:20,43` | `src/apps/meta_db/main.go:19` | ✅ |
| 5006 MetaDB FB | `level4-deployment.puml:21,43` | `src/apps/meta_db/flatbuffer_tcp.go:14` | ✅ |
| 5200 ESS | `level4-deployment.puml:22,42,70` | `src/apps/entity_state_store/main.cpp:257-258` | ✅ |
| 7777/7778 gateway | `level4-deployment.puml:16-17,38,65`; `level3-gateway.puml:24-25` | `src/apps/gateway/main.cpp:31-32` | ✅ |
| **5555 RecipeManager** | `level2-container.puml:49`, `level4-deployment.puml:23,46`, `README.md:115` | **no listener anywhere** | ❌ **C4-01** |
| **55555 client** | `level2-container.puml:62`, `level4-deployment.puml:40` | client *connects out* to 7777/7778; `src/apps/game_client/main.cpp:52-53`; no `:55555` in the tree | ❌ **C4-13** |
| **192 KB chunk** | `level4-deployment.puml:31`, `level3-chunkstore.puml:19`, `level2-container.puml:31` | palette-encoded, ~260 B (`SectionCodec.h:5-22`, `chunkd_load_test.cpp:96`) | ❌ **C4-14** |

### C4-13 (MEDIUM) — the client is drawn as listening on `:55555`; it is a TCP client

`doc/c4/level2-container.puml:62` — `Container(client, "GameClient", "C++ (bgfx+GLFW+ImGui)\n:55555", ...)`
`doc/c4/level4-deployment.puml:40` — `Container(client_d, "client", "C++ :55555", "5. GameClient\nbgfx + GLFW")`

`grep -rn "55555" --include=*.cpp --include=*.h --include=*.go src/` → no hits.
`src/apps/game_client/main.cpp:52-53` sets `server_port = 7777` / `bulk_port = 7778` and
the client dials out. It binds nothing. The `:55555` appears to be a leftover from a
different era's convention (bgfx's remote-debug port range).

### C4-14 (MEDIUM) — the 192 KB chunk value is wrong in three diagrams

`doc/c4/level4-deployment.puml:31` (`"значение: Chunk (192KB)"`),
`level3-chunkstore.puml:19` (`"32³ блоков = 192 KB\n3 слоя: blocks[16bit] + meta[8bit] + mb_id[32bit]"`),
`level2-container.puml:31` (`"Чанки: blocks[] + meta[] + mb_id[]\n192 KB на чанк"`).

The real format is palette-encoded per 16³ section — `src/apps/chunk_store/Storage/SectionCodec.h:5-15`
documents `[magic "GCHK"][ver 1][sec_cnt 8] sections[8]: [palette_size u16][palette][bits_per_index u8][indices][meta_count][meta][mb_count][mb]`,
with `SEC_SZ=16, SEC_VOL=4096, SEC_CNT=8` (`:20-22`), and the in-memory form is a tagged
union with sparse meta (`cache/MutableSection.h:10,40-45`). The repo's own benchmark
says `~260 bytes palette-encoded` (`Storage/chunkd_load_test.cpp:96`).

The same figure is stale in `AGENTS.md:158`, `README.md:110`, `openspec/project.md:74`,
`doc/diff-protocol.md:7,13`, `openspec/specs/protocol/spec.md:24` — 9 files, one wrong value.

---

## 4. Service inventory and naming

### C4-15 (MEDIUM) — process names in `level4-deployment.puml` are abbreviations that match no target

`doc/c4/level4-deployment.puml:36-47`: `routerd` ✅, `chunk_d "chunkd"` ✅,
`gateway_d "gatewayd"` ✅, `simcore_d "simcored"` ❌ (real: `simcored_exec`),
`client_d "client"` ❌ (real: `gameclientd`), `ess_d "entityd"` ❌ (real: `entitystated`),
`metadb_d "metadbd"` ✅, `worldgen_d "worldgend"` ❌ (no such process — C4-03),
`pipenet_d "piped"` ❌ (real: `pipe_networkd`), `recipeman_d "reciped"` ✅,
`spatial_d "spatiald"` ❌ (real: `spatialindexd`, and not built at all — C4-02).

7 of 11 process labels do not match a `CMakeLists.txt` `add_executable` name. The
underlying targets are listed in `stale-docs.md` §5.

### C4-16 (LOW) — the "13 services" count in `doc/c4/README.md:11,46` is not reproducible

`doc/c4/README.md:11` — `level2-container.puml # L2 — Containers (13 сервисов)`
`doc/c4/README.md:46` — `| 2 | **Container** | 13 сервисов, TCP соединения, MessageRouter pub/sub |`

`level2-container.puml:3` — `' All 13 services, their TCP connections, protocols, and data stores`

Counting the `Container(...)` declarations inside the server boundary
(`level2-container.puml:24-57`): router, gateway, chunkstore, worldgen, simcore,
spatial, pipenet, ess, recipeman, metadb, validation, storage_ifaces = **12**, plus
`client` at `:62` = 13. So the count is arithmetically consistent — but 4 of the 13
have **no process at all** (worldgen = library, spatial = unbuilt stub, validation =
unbuilt executable, storage_ifaces = headers). The real number of runnable daemons is
**9**, or 8 if you count only the C++ ones in `cmake-build-debug/src/apps/`.

`AGENTS.md:33` says "9 runnable daemons"; `README.md:171` says "10 runnable daemons + 2
stubs"; `README.md:6` says "9 daemons"; `doc/c4/README.md` says "13 сервисов". Four
different numbers across four files, none reconcilable without reading the tree.

### C4-17 (LOW) — "FastNoiseLite" is the wrong noise library in two diagrams

`doc/c4/level2-container.puml:33` — `"C++ (FastNoiseLite)"`
`doc/c4/level3-tiny-services.puml:16` — `Container_Boundary(wg, "WorldGenerator (C++ — FastNoiseLite, библиотека)")`

The tree uses **fastnoise2**: `conanfile.txt:12` `fastnoise2/1.1.1`,
`src/apps/world_generator/OreGenerator.cpp:3` `#include "FastNoise/FastNoise.h"`,
`SurfaceHeights.cpp:3` same. FastNoiseLite is a different single-header library and is
not in the tree. `AGENTS.md:183` says fastnoise2 and is right.

### C4-18 (LOW) — `AGENTS.md:141` claims the diagrams are "15 ECS-систем" verified against `registerSystem`; that count is **correct**

`doc/c4/README.md:108` — "**SimulationCore**: 15 ECS-систем (сверено с `simulation_core/main.cpp` registerSystem)"

```
$ grep -c "registerSystem" src/apps/simcore/main.cpp
15
$ grep -o "simcore::[A-Za-z]*System" src/apps/simcore/main.cpp | sort -u
AdjacencyTransferSystem, BatteryBufferSystem, BoilerSystem, CoolantSystem,
CreativeGeneratorSystem, DrillSystem, EBFSystem, ExplosionSystem, GeneratorSystem,
LargeBoilerSystem, LCRSystem, MachineSystem, RotareGeneratorSystem,
SteamTurbineSystem, TransformerSystem
```

15, and `level3-simcore.puml:21` names 14 of them by hand in a list that **omits
`MachineSystem`** while including all other 14. So the count is right, the hand-written
enumeration at `level3-simcore.puml:21` is one short. (`level3-simcore.puml:19` also
says "14 компонентов, 15 систем" — the components figure is unverified here.)

Minor nit: `doc/c4/README.md:108` cites the path `simulation_core/main.cpp`, which
does not exist; the file is `src/apps/simcore/main.cpp`. A leftover from the pre-rename
`src/services/simulation_core/` layout — the same rename that left `ROADMAP.md` with
11 `src/services/*` paths.

### C4-19 (LOW) — `level2-container.puml:107` EntityStateStore → ChunkStore edge is real, but the two diagrams disagree about who else calls it

`doc/c4/level2-container.puml:107` — `Rel(ess, chunkstore, "ChunkStoreClient\nдля сущностей к блок данным", $tags="tag_rpc")`

Verified: `src/apps/entity_state_store/main.cpp:14,185,245-246,255` — it constructs a
`ChunkStoreClient` and connects to `127.0.0.1:5001`. **The edge is correct.** But
`level4-deployment.puml:42` labels the process `entityd` and `:71` gives the same edge,
while `level2-container.puml:45` labels it `EntityStateStore` with `:5200`. Naming
inconsistency only, no topology error.

### C4-20 (LOW) — the diagram omits SimCore's hard-coded EntityStateStore connection

`src/apps/simcore/main.cpp:285` — `entityStateClient->Connect("127.0.0.1", 5200);`
**hard-coded**, not flag-driven, unlike its chunkstore/router addresses at `:119-123`.

`level2-container.puml:97` does draw `Rel(simcore, ess, "TCP RPC: entity.state.get/set", $tags="tag_rpc")` — so the edge exists. But neither `level2-container.puml` nor `level4-deployment.puml` notes that the address cannot be reconfigured, which matters for the
`add-multiplayer-foundation` / `add-chunk-interest-aggregator` work in flight
(`openspec/changes/`). Not a diagram error per se; a diagram gap on a known-hardcoded
value. Flagged because a reader planning multi-host deployment will be misled.

---

## 5. Verified CORRECT in `doc/c4/`

Do not touch these:

1. **`level3-gateway.puml:24-25`** — ctrl `:7777` / bulk `:7778`, and the message split
   (`PlayerAction / InventoryAction / CraftRequest / SetMachineSlot` on ctrl;
   `ChunkData / EntitySnapshot / CompressedChunkData` on bulk) matches
   `src/apps/gateway/main.cpp:31-32` and the client defaults at
   `src/apps/game_client/main.cpp:52-53`. ✅
2. **All 7 real ports** — 4000, 5001, 5005, 5006, 5200, 7777, 7778 — each traced to a
   `main()` default. ✅
3. **`level3-meta-db.puml:14,17,38,39`** — `:5005` JSON, `:5006` FlatBuffers/MetaDBFrame,
   and the `player.joined → player.inventory.load` / `player.left → player.position.load`
   flows all verified against `main.go:19`, `flatbuffer_tcp.go:14,147`,
   `router_client.go:386,405`. ✅
4. **`level3-meta-db.puml:16` topic list** — exactly matches
   `src/apps/meta_db/router_client.go:103-114`, all 10. ✅
5. **`level3-simcore.puml:21` "15 ECS Systems"** (count) and `doc/c4/README.md:108` —
   15 `registerSystem` calls confirmed. ✅ (the hand-listed names omit `MachineSystem`)
6. **`level2-container.puml:96`** `Rel(simcore, chunkstore, "RPC: GetBlock / SetBlockMeta / AsyncGetChunk")` —
   `src/apps/simcore/main.cpp:288` `ChunkStoreRepository(chunkstoreClient)` over
   `IoUringChunkClient`. ✅
7. **`doc/c4/README.md:112`** "MetaDB: JSON API :5005 + FlatBuffers RPC :5006 (MetaDBFrame)" ✅
8. **`doc/c4/README.md:109-111`** "51 тип GatewayMsg" ✅ — `src/common/GatewayMsg.h:73`.
9. **`doc/c4/README.md:116-117`** multiplayer marked as Planned A.1 on the gateway and
   deployment diagrams — correct and current; `openspec/changes/add-multiplayer-foundation/`
   exists and is unlanded. ✅
10. **The `$tags="planned"` mechanism itself** — SpatialIndex and the multiplayer fanout
    are correctly *flagged* as planned. The error is in the prose inside those elements,
    not the annotation. ✅

---

## 6. Summary

| ID | Severity | Finding |
|----|----------|---------|
| C4-01 | CRITICAL | `:5555` RecipeManager port does not exist — no listener in the tree (4 files) |
| C4-02 | CRITICAL | SpatialIndex drawn as a live R-tree peer; it is a 2-line unbuilt stub |
| C4-03 | CRITICAL | WorldGenerator drawn as a networked process; it is a static library linked into chunkd |
| C4-04 | CRITICAL | Validation drawn as a static lib with 2 inbound RPC edges; it is an unbuilt *executable* |
| C4-05 | HIGH | All 4 drawn ChunkStore topics are wrong (`chunk.requests` / `world.chunk.loaded.compressed` are the real ones) |
| C4-06 | HIGH | MetaDB outbound topic set is missing 5 publications (inbound is exactly right) |
| C4-07 | MEDIUM | `multiblock.*` → real names `sim.multiblock.created/destroyed` |
| C4-08 | MEDIUM | `pipe_network.*` does not exist; pipenet has 13 subscriptions across `energy/fluid/item/pipe/world` |
| C4-09 | MEDIUM | SimCore→PipeNetwork drawn as TCP RPC; it is pub/sub via `PipeEnergyClient` |
| C4-10 | MEDIUM | Gateway's 26 router subscriptions reduced to `world.* / player.*` in the diagram |
| C4-11 | MEDIUM | Priority-tier map wrong: 4 PrioNormal topics missing, `meta_db.*` filed as PrioLow |
| C4-12 | LOW | `entity.state.*` family is larger than drawn, plus an undocumented `world.blocks.changed` subscription |
| C4-13 | MEDIUM | Client drawn as listening on `:55555`; it is a TCP client on 7777/7778 |
| C4-14 | MEDIUM | 192 KB chunk value wrong in 3 diagrams (real: palette-encoded, ~260 B) |
| C4-15 | MEDIUM | 7 of 11 process labels in `level4-deployment.puml` match no CMake target |
| C4-16 | LOW | "13 services" not reproducible; 4 of the 13 have no process; 4 numbers across 4 files |
| C4-17 | LOW | "FastNoiseLite" is the wrong noise library (tree uses fastnoise2) |
| C4-18 | LOW | "15 ECS systems" **correct**, but the hand-list omits `MachineSystem`; stale `simulation_core/` path |
| C4-19 | LOW | ESS→ChunkStore edge verified correct; naming inconsistent between diagrams |
| C4-20 | LOW | SimCore's `:5200` is hard-coded; neither diagram notes the un-reconfigurable address |
| — | ✅ | 10 claims verified correct, listed in §5 |

**Bottom line for the follow-up task:** the *annotations* (planned/WIP flags, the
colour legend, the level structure, the MetaDB internals, the gateway port split) are
sound and worth keeping. What is broken is the **service-inventory layer** — 4 of the
"13 services" have no process — and the **topic layer**, which is stale almost
everywhere it appears. The highest-value single fix is deleting `:5555` and the three
phantom service edges (C4-01 through C4-04); that alone removes 4 CRITICAL findings
and makes `doc/c4/` defensible as "authoritative" for the first time.
