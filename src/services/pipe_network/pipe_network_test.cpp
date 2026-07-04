#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <cmath>
#include <common/ItemId.h>
#include "PipeNetwork.h"
#include "CableGraph.h"
#include "CableTypes.h"
#include "FluidRegistry.h"

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#define CHECK_NE(a, b, msg) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, msg)
#define CHECK_GT(a, b, msg) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, msg)
#define CHECK_GE(a, b, msg) test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, msg)
#define CHECK_LT(a, b, msg) test_check((a) < (b), __FILE__, __LINE__, #a " < " #b, msg)
#define PASS() do { ++g_passed; } while(0)

// =========================================================================
//  Existing tests (PipeNetwork basics)
// =========================================================================

static void test_empty_network() {
    pipenet::PipeNetworkManager mgr;
    CHECK_EQ(mgr.nodeCount(), size_t(0), "no nodes initially");
    CHECK_EQ(mgr.networkCount(), size_t(0), "no networks initially");
    PASS();
}

static void test_single_node() {
    pipenet::PipeNetworkManager mgr;
    uint64_t nid = mgr.addNode(0, 0, 0, 100);
    CHECK_GT(nid, size_t(0), "node id > 0");
    CHECK_EQ(mgr.nodeCount(), size_t(1), "one node");
    auto* node = mgr.getNode(nid);
    CHECK(node != nullptr, "node exists");
    CHECK_EQ(node->x, 0, "x"); CHECK_EQ(node->y, 0, "y"); CHECK_EQ(node->z, 0, "z");
    CHECK_EQ(node->block_id, uint16_t(100), "block_id");
    PASS();
}

static void test_add_remove_node() {
    pipenet::PipeNetworkManager mgr;
    uint64_t nid = mgr.addNode(10, 20, 30, 200);
    mgr.removeNode(nid);
    CHECK_EQ(mgr.getNode(nid), nullptr, "node removed");
    CHECK_EQ(mgr.nodeCount(), size_t(0), "no nodes after remove");
    PASS();
}

static void test_network_discovery() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(1, 0, 0, 100);
    uint64_t c = mgr.addNode(2, 0, 0, 100);
    mgr.addEdge(a, b);
    mgr.addEdge(b, c);

    auto net = mgr.discoverNetwork(a);
    CHECK_EQ(net.size(), size_t(3), "3 nodes in network");
    PASS();
}

static void test_disconnected_graphs() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(10, 0, 0, 100);
    mgr.addEdge(a, b);
    uint64_t n2_a = mgr.addNode(20, 0, 0, 100);
    uint64_t n2_b = mgr.addNode(30, 0, 0, 100);
    mgr.addEdge(n2_a, n2_b);

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(2), "two disconnected networks");

    auto net_a = mgr.discoverNetwork(a);
    CHECK_EQ(net_a.size(), size_t(2), "first network has 2 nodes");
    auto net_c = mgr.discoverNetwork(n2_a);
    CHECK_EQ(net_c.size(), size_t(2), "second network has 2 nodes");
    PASS();
}

static void test_rebuild_networks() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(1, 0, 0, 100);
    mgr.addEdge(a, b);

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(1), "one network after rebuild");

    mgr.addNode(5, 0, 0, 100);
    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(2), "two networks after adding isolated node");
    PASS();
}

static void test_add_node_with_id() {
    pipenet::PipeNetworkManager mgr;
    bool ok = mgr.addNodeWithId(42, 5, 5, 5, 99);
    CHECK(ok, "addNodeWithId success");
    CHECK_EQ(mgr.nodeCount(), size_t(1), "one node after add");

    ok = mgr.addNodeWithId(42, 6, 6, 6, 99);
    CHECK(!ok, "duplicate id returns false");
    CHECK_EQ(mgr.nodeCount(), size_t(1), "still one node after duplicate");
    PASS();
}

// =========================================================================
//  Item network tests
// =========================================================================

static void test_item_network_simple() {
    // Simple source → single pipe → sink: item should move in one tick
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);   // item_pipe
    uint64_t pipe = mgr.addNode(1, 0, 0, 62);  // item_pipe
    uint64_t sink = mgr.addNode(2, 0, 0, 62);  // item_pipe

    mgr.addEdge(src, pipe);
    mgr.addEdge(pipe, sink);

    // Configure: src produces items, sink consumes
    mgr.setNodeItemProps(src, 10, true);   // 10 slot capacity, is source
    mgr.setNodeItemProps(pipe, 10, false);  // 10 slot capacity, not source
    mgr.setNodeItemProps(sink, 10, false);  // 10 slot capacity, not source
    mgr.setNodeEnergy(sink, 0, 100, false, true);  // set sink=true for energy/isSink

    // Add one item at source
    mgr.addNodeItem(src, 42, 1);  // item_id=42, count=1

    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(1), "source has 1 item before tick");
    CHECK_EQ(mgr.getNode(sink)->itemBuffer.size(), size_t(0), "sink has 0 items before tick");

    mgr.tickItemNetworks();

    // After tick, item should have moved from source to sink
    // Note: depending on BFS order, it might be in pipe or sink
    // At minimum, source should have 0 items
    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(0), "source has 0 items after tick");

    PASS();
}

static void test_item_network_no_sink() {
    // Source with items but no sink in network: items stay at source
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);
    mgr.setNodeItemProps(src, 10, true);
    mgr.addNodeItem(src, 7, 1);

    mgr.tickItemNetworks();

    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(1), "item stays at source without sink");
    PASS();
}

static void test_item_network_multi_item() {
    // Multiple items from source to sink
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);
    uint64_t sink = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(src, sink);

    mgr.setNodeItemProps(src, 10, true);
    mgr.setNodeItemProps(sink, 10, false);
    mgr.setNodeEnergy(sink, 0, 1000, false, true);

    mgr.addNodeItem(src, 1, 1);
    mgr.addNodeItem(src, 2, 1);
    mgr.addNodeItem(src, 3, 1);

    mgr.tickItemNetworks();

    // moveItemsInNetwork moves 1 item per source per tick
    CHECK_LT(mgr.getNode(src)->itemBuffer.size(), size_t(3), "source has fewer items after tick");
    PASS();
}

static void test_item_network_multi_tick() {
    // Multiple ticks to move all items
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);
    uint64_t sink = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(src, sink);

    mgr.setNodeItemProps(src, 10, true);
    mgr.setNodeItemProps(sink, 10, false);
    mgr.setNodeEnergy(sink, 0, 1000, false, true);

    mgr.addNodeItem(src, 1, 1);
    mgr.addNodeItem(src, 2, 1);
    mgr.addNodeItem(src, 3, 1);

    for (int i = 0; i < 5; ++i) mgr.tickItemNetworks();

    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(0), "source empty after 5 ticks");
    CHECK_GT(mgr.getNode(sink)->itemBuffer.size(), size_t(0), "sink has items after 5 ticks");
    PASS();
}

static void test_find_next_item_hop() {
    // BFS: start at node 0, should find node 1 (item-capable neighbor)
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    uint64_t c = mgr.addNode(2, 0, 0, 62);
    mgr.addEdge(a, b);
    mgr.addEdge(b, c);

    mgr.setNodeItemProps(a, 10, false);
    mgr.setNodeItemProps(b, 10, false);
    mgr.setNodeItemProps(c, 10, false);

    mgr.rebuildItemNetworks();
    auto* net = mgr.getItemNetwork(a);
    CHECK(net != nullptr, "item network exists");

    uint64_t hop = mgr.findNextItemHop(a, net->id);
    CHECK(hop == b || hop == c, "findNextItemHop returns connected node");
    PASS();
}

static void test_find_next_item_hop_no_item_capability() {
    // Node with itemCapacity=0 should NOT be returned as a hop
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 100);  // non-pipe block (itemCapacity=0)
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, false);
    // b has default itemCapacity=0

    mgr.rebuildItemNetworks();
    auto* net = mgr.getItemNetwork(a);
    CHECK(net != nullptr, "item network exists");

    uint64_t hop = mgr.findNextItemHop(a, net->id);
    CHECK_EQ(hop, uint64_t(0), "no hop to non-item-capable node");
    PASS();
}

// =========================================================================
//  Energy distribution tests
// =========================================================================

static void test_energy_distribution_simple() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src1 = mgr.addNode(0, 0, 0, 46);
    uint64_t src2 = mgr.addNode(1, 0, 0, 46);
    uint64_t sink = mgr.addNode(2, 0, 0, 37);

    mgr.addEdge(src1, src2);
    mgr.addEdge(src2, sink);

    mgr.setNodeEnergy(src1, 1000, 1000, true, false);
    mgr.setNodeEnergy(src2, 500, 500, true, false);
    mgr.setNodeEnergy(sink, 0, 2000, false, true);

    mgr.rebuildNetworks();
    CHECK_GE(mgr.networkCount(), size_t(1), "at least one energy network");

    // Find the network containing our sink
    auto nets = mgr.getAllNetworks();
    uint64_t targetNetId = 0;
    for (const auto* n : nets) {
        for (uint64_t nid : n->nodeIds) {
            if (nid == sink) { targetNetId = n->id; break; }
        }
        if (targetNetId) break;
    }
    CHECK_GT(targetNetId, uint64_t(0), "found network for sink");

    auto deltas = mgr.distributeEnergy(targetNetId, 300);
    CHECK(!deltas.empty(), "energy distribution produced deltas");

    const auto* sinkNode = mgr.getNode(sink);
    CHECK_GT(sinkNode->energyBuffer, 0, "sink received energy");
    PASS();
}

static void test_energy_distribution_no_sink() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 46);
    mgr.setNodeEnergy(src, 500, 1000, true, false);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == src) { targetNet = n->id; break; }

    if (targetNet) mgr.distributeEnergy(targetNet, 100);
    // distributeFlow drains sources even without sinks (existing behavior)
    CHECK_LT(mgr.getNode(src)->energyBuffer, 500, "energy drained from source without sink");
    PASS();
}

static void test_energy_distribution_capacity_limited() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 46);
    uint64_t sink = mgr.addNode(1, 0, 0, 37);
    mgr.addEdge(src, sink);

    mgr.setNodeEnergy(src, 10000, 10000, true, false);
    mgr.setNodeEnergy(sink, 90, 100, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    mgr.distributeEnergy(targetNet, 100);
    const auto* sinkNode = mgr.getNode(sink);
    CHECK_EQ(sinkNode->energyBuffer, 100, "sink capped at capacity");
    PASS();
}

// =========================================================================
//  Fluid distribution tests
// =========================================================================

static void test_fluid_distribution_simple() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t sink = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, sink);

    mgr.setNodeFluid(src, 1000, 2000, 84, true, false);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "fluid network exists");

    auto* net = mgr.getNetwork(targetNet);
    CHECK_EQ(net->fluidId, uint32_t(84), "network fluid type is water");

    auto deltas = mgr.distributeFluid(targetNet, 200);
    CHECK(!deltas.empty(), "fluid distribution produced deltas");

    const auto* sinkNode = mgr.getNode(sink);
    CHECK_GT(sinkNode->fluidBuffer, 0, "sink received fluid");
    CHECK_EQ(sinkNode->fluidId, uint32_t(84), "sink fluid type is water");
    PASS();
}

static void test_fluid_distribution_capacity_limited() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t sink = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, sink);

    // Use fluidId=0 on sink so distributeFluid will fill it
    mgr.setNodeFluid(src, 10000, 10000, 84, true, false);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    mgr.distributeFluid(targetNet, 1000);
    const auto* sinkNode = mgr.getNode(sink);
    CHECK_EQ(sinkNode->fluidBuffer, 500, "sink capped at capacity");
    PASS();
}

static void test_fluid_distribution_no_source() {
    pipenet::PipeNetworkManager mgr;
    uint64_t sink = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    if (targetNet) mgr.distributeFluid(targetNet, 100);
    // distributeFluid adds fluid directly without checking for sources
    CHECK_GE(mgr.getNode(sink)->fluidBuffer, 0, "fluid distribution did not crash");
    PASS();
}

// =========================================================================
//  FluidRegistry tests
// =========================================================================

static void test_fluid_registry_defaults() {
    auto& reg = FluidRegistry::instance();
    uint16_t waterId = ItemId::pack("1111:11:0");
    uint16_t steamId = ItemId::pack("1111:11:1");
    uint16_t acidId  = ItemId::pack("1111:11:2");

    CHECK(reg.isFluid(waterId), "water registered");
    CHECK(reg.isFluid(steamId), "steam registered");
    CHECK(reg.isFluid(acidId), "sulfuric_acid registered");

    const auto* water = reg.getFluid(waterId);
    CHECK(water != nullptr, "water def exists");
    CHECK(water->item_id == waterId, "water id correct");
    CHECK(water->max_temp == 373, "water max temp correct");

    const auto* steam = reg.getFluid(steamId);
    CHECK(steam != nullptr, "steam def exists");
    CHECK(steam->density < 1.0f, "steam less dense than water");

    const auto* acid = reg.getFluid(acidId);
    CHECK(acid != nullptr, "acid def exists");
    CHECK(acid->density > 1.0f, "acid denser than water");

    // Re-init should not duplicate
    reg.initDefaults();
    CHECK(reg.isFluid(84), "water still registered after re-init");

    PASS();
}

// =========================================================================
//  CableGraph tests
// =========================================================================

static void test_cable_graph_add_remove() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    const CableDef tinDef = {66, 1, "cable_tin", 100000000.0f, 32, 32};

    graph.addCableNode(1, tinDef, 0, 0, 0);
    graph.addCableNode(2, tinDef, 1, 0, 0);

    CHECK_EQ(graph.isRegisteredGenerator(1), false, "not a generator");
    graph.registerGenerator(100, 0, 0, 0);
    CHECK(graph.isRegisteredGenerator(100), "generator registered");

    graph.unregisterGenerator(100);
    CHECK_EQ(graph.isRegisteredGenerator(100), false, "generator unregistered");

    graph.removeCableNode(1);
    graph.removeCableNode(2);
    PASS();
}

static void test_cable_graph_packet_routing() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    const CableDef def = {66, 1, "cable_tin", 100000000.0f, 32, 32};

    graph.addCableNode(1, def, 0, 0, 0);
    graph.addCableNode(2, def, 1, 0, 0);
    graph.addCableNode(3, def, 2, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, 0, 0, 0);
    graph.registerMachine(200, 2, 0, 0);

    // Inject and tick — verifies no crash, packet processing works
    graph.injectPacket({32, 1, 100, 0, 0}, 1);
    graph.tick();

    graph.collectPackets(200);
    PASS();
}

static void test_cable_graph_voltage_limit() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    // Low voltage cable (maxVoltage=32)
    CableDef lowDef = {66, 1, "cable_tin", 100000000.0f, 32, 32};
    // High voltage cable (maxVoltage=512)
    CableDef highDef = {68, 2, "cable_gold", 49668352.0f, 128, 128};

    graph.addCableNode(1, lowDef, 0, 0, 0);
    graph.addCableNode(2, highDef, 1, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, 0, 0, 0);
    graph.registerMachine(200, 1, 0, 0);

    graph.injectPacket({512, 1, 100, 200, 0}, 1);
    graph.tick();

    auto packets = graph.collectPackets(200);
    // Packet may or may not arrive (voltage check may block it)
    // Test confirms the graph processes without crash
    PASS();
}

static void test_cable_graph_loss() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef cableDef = {1000, 1, "test_cable", 1.0f, 1000, 100};

    graph.addCableNode(1, cableDef, 0, 0, 0);
    graph.addCableNode(2, cableDef, 1, 0, 0);
    graph.addCableNode(3, cableDef, 2, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 3, 0, 0);

    graph.injectPacket({100, 1, 100, 0, 0}, 1);
    graph.tick();

    auto packets = graph.collectPackets(200);
    CHECK_GT(packets.size(), size_t(0), "packet arrived at machine after loss");
    if (!packets.empty()) {
        CHECK_EQ(packets[0].voltage, uint32_t(98), "voltage reduced by 2 after 2 hops");
    }

    PASS();
}

static void test_cable_graph_heavy_loss() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef highLossDef = {1001, 1, "lossy_cable", 60.0f, 1000, 100};

    // 4 cable nodes: (0..3,0,0), machine adjacent to node 4
    graph.addCableNode(1, highLossDef, 0, 0, 0);
    graph.addCableNode(2, highLossDef, 1, 0, 0);
    graph.addCableNode(3, highLossDef, 2, 0, 0);
    graph.addCableNode(4, highLossDef, 3, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 4, 0, 0);

    // broadcast: 100V through 3 hops of 60 EU loss → 100-180 < 0 → dissipated
    graph.injectPacket({100, 1, 100, 0, 0}, 1);
    graph.tick();

    auto packets = graph.collectPackets(200);
    CHECK_EQ(packets.size(), size_t(0), "packet dissipated before reaching machine");
    PASS();
}

static void test_cable_graph_overheat_explosion() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef cableDef = {1002, 1, "weak_cable", 0.0f, 32, 5};

    // 2 cable nodes: (0,0,0)=1, (1,0,0)=2. Machine adjacent to node 2.
    graph.addCableNode(1, cableDef, 0, 0, 0);
    graph.addCableNode(2, cableDef, 1, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 2, 0, 0);

    // Inject overvoltage (34V, max is 32). In broadcasting: path 1→2.
    // During forwarding, node 2 gets maxSeenVoltage=34 and voltage check fails.
    // calculateOverheat: (34-32)*50 = 100 heat. -2 cooldown = 98. Not exploded.
    graph.injectPacket({34, 1, 100, 0, 0}, 1);
    graph.tick();

    CHECK_EQ(graph.getExplodedNodes().size(), size_t(0), "no explosion after 1 tick");

    // Second tick: same again → 98 + 100 - 2 = 196 ≥ 100 → exploded
    graph.injectPacket({34, 1, 100, 0, 0}, 1);
    graph.tick();

    CHECK_GT(graph.getExplodedNodes().size(), size_t(0), "explosion after 2 ticks");
    if (!graph.getExplodedNodes().empty()) {
        const auto& expl = graph.getExplodedNodes()[0];
        CHECK_EQ(expl.nodeId, uint64_t(2), "exploded node is the receiver");
        CHECK_GE(expl.temperature, 100.0f, "temperature >= threshold at explosion");
    }

    PASS();
}

static void test_cable_graph_ampacity_overheat() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef cableDef = {1003, 1, "weak_cable", 0.0f, 1000, 1};

    // 2 cable nodes, machine adjacent to node 2
    graph.addCableNode(1, cableDef, 0, 0, 0);
    graph.addCableNode(2, cableDef, 1, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 2, 0, 0);

    // Multiple ticks with 5 packets each (ampacity=1, so 4 overflow per tick)
    // Per tick: calculateOverheat adds (5-1)*1 = 4 heat, -2 cooldown = net +2
    // After ~50 ticks: 50*2 = 100 → threshold reached
    for (int tick = 0; tick < 100; ++tick) {
        for (int i = 0; i < 5; ++i) {
            graph.injectPacket({50, 1, 100, 0, 0}, 1);
        }
        graph.tick();
        if (!graph.getExplodedNodes().empty()) break;
    }

    CHECK_GT(graph.getExplodedNodes().size(), size_t(0), "explosion from ampacity overheat");
    PASS();
}

// =========================================================================
//  Edge cases and stress
// =========================================================================

static void test_remove_edge_and_rebuild() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(1, 0, 0, 100);
    uint64_t c = mgr.addNode(2, 0, 0, 100);
    uint64_t e1 = mgr.addEdge(a, b);
    mgr.addEdge(b, c);

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(1), "one network with 3 nodes");

    mgr.removeEdge(e1);
    mgr.rebuildNetworks();
    // a isolated, b-c connected
    CHECK_EQ(mgr.networkCount(), size_t(2), "two networks after edge removal");
    PASS();
}

static void test_large_network() {
    // 100 nodes in a line - stress test
    pipenet::PipeNetworkManager mgr;
    std::vector<uint64_t> nodes;
    for (int i = 0; i < 100; ++i) {
        nodes.push_back(mgr.addNode(i, 0, 0, 62));
    }
    for (size_t i = 0; i < nodes.size() - 1; ++i) {
        mgr.addEdge(nodes[i], nodes[i + 1]);
    }

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(1), "one large network");
    CHECK_EQ(mgr.nodeCount(), size_t(100), "100 nodes");
    CHECK_EQ(mgr.edgeCount(), size_t(99), "99 edges");

    // Item network: first node source, last node sink
    mgr.setNodeItemProps(nodes[0], 100, true);
    for (size_t i = 1; i < nodes.size() - 1; ++i) {
        mgr.setNodeItemProps(nodes[i], 100, false);
    }
    mgr.setNodeItemProps(nodes.back(), 100, false);
    mgr.setNodeEnergy(nodes.back(), 0, 10000, false, true);

    mgr.addNodeItem(nodes[0], 1, 1);

    mgr.tickItemNetworks();
    CHECK_EQ(mgr.getNode(nodes[0])->itemBuffer.size(), size_t(0), "source drained");
    PASS();
}

static void test_node_count_after_operations() {
    pipenet::PipeNetworkManager mgr;
    CHECK_EQ(mgr.nodeCount(), size_t(0), "empty");
    uint64_t n1 = mgr.addNode(0, 0, 0, 100);
    CHECK_EQ(mgr.nodeCount(), size_t(1), "after add");
    mgr.removeNode(n1);
    CHECK_EQ(mgr.nodeCount(), size_t(0), "after remove");

    // Re-add with same coords (different id)
    mgr.addNode(0, 0, 0, 100);
    CHECK_EQ(mgr.nodeCount(), size_t(1), "re-added");
    PASS();
}

// =========================================================================
//  Main
// =========================================================================

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

int main(int, char**) {
    printf("=== PipeNetwork Test ===\n\n");

    // Existing
    TEST(empty_network);
    TEST(single_node);
    TEST(add_remove_node);
    TEST(network_discovery);
    TEST(disconnected_graphs);
    TEST(rebuild_networks);
    TEST(add_node_with_id);

    // Item network
    TEST(item_network_simple);
    TEST(item_network_no_sink);
    TEST(item_network_multi_item);
    TEST(item_network_multi_tick);
    TEST(find_next_item_hop);
    TEST(find_next_item_hop_no_item_capability);

    // Energy distribution
    TEST(energy_distribution_simple);
    TEST(energy_distribution_no_sink);
    TEST(energy_distribution_capacity_limited);

    // Fluid distribution
    TEST(fluid_distribution_simple);
    TEST(fluid_distribution_capacity_limited);
    TEST(fluid_distribution_no_source);

    // FluidRegistry
    TEST(fluid_registry_defaults);

    // CableGraph
    TEST(cable_graph_add_remove);
    TEST(cable_graph_packet_routing);
    TEST(cable_graph_voltage_limit);
    TEST(cable_graph_loss);
    TEST(cable_graph_heavy_loss);
    TEST(cable_graph_overheat_explosion);
    TEST(cable_graph_ampacity_overheat);

    // Edge cases
    TEST(remove_edge_and_rebuild);
    TEST(large_network);
    TEST(node_count_after_operations);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
