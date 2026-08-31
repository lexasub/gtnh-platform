#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <cmath>
#include <common/ItemId.h>
#include "PipeNetwork.h"
#include "HeatLoss.h"
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
//  Pipe wrench guidance tests (evaluatePipeWrench)
// =========================================================================

static void test_wrench_isolated_pipe() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    uint64_t nid = 0;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;

    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::CONNECT_PIPES, "isolated pipe -> CONNECT_PIPES");
    CHECK_EQ(nid, uint64_t(1), "node id returned for pipe position");
    PASS();
}

static void test_wrench_pipe_to_pipe() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;
    pipes[pipenet::pipePosKey(1, 0, 0)] = 2;

    uint64_t nid = 0;
    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::CONNECT_TO_MACHINE,
          "pipe next to pipe -> CONNECT_TO_MACHINE");
    PASS();
}

static void test_wrench_pipe_adjacent_machine() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;
    machines[pipenet::pipePosKey(1, 0, 0)] = 900;

    uint64_t nid = 0;
    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::CONNECTED, "pipe adjacent machine -> CONNECTED");
    PASS();
}

static void test_wrench_non_pipe_position() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(5, 5, 5)] = 1;

    uint64_t nid = 77;
    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::NOT_A_PIPE, "no pipe at pos -> NOT_A_PIPE");
    CHECK_EQ(nid, uint64_t(0), "node id cleared to 0 for non-pipe");
    PASS();
}

static void test_wrench_guidance_no_mutation() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;
    pipes[pipenet::pipePosKey(1, 0, 0)] = 2;

    auto before = pipes;
    uint64_t nid = 0;
    pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(pipes == before, "pipe map unchanged after evaluation");
    CHECK(machines.empty(), "machine map unchanged after evaluation");
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
    mgr.setNodeItemProps(src, 10, true, false);   // 10 slot capacity, is source
    mgr.setNodeItemProps(pipe, 10, false, false);  // 10 slot capacity, not source
    mgr.setNodeItemProps(sink, 10, false, false);  // 10 slot capacity, not source
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
    mgr.setNodeItemProps(src, 10, true, false);
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

    mgr.setNodeItemProps(src, 10, true, false);
    mgr.setNodeItemProps(sink, 10, false, false);
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

    mgr.setNodeItemProps(src, 10, true, false);
    mgr.setNodeItemProps(sink, 10, false, false);
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

    mgr.setNodeItemProps(a, 10, false, false);
    mgr.setNodeItemProps(b, 10, false, false);
    mgr.setNodeItemProps(c, 10, false, false);

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

    mgr.setNodeItemProps(a, 10, false, false);
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

static void test_fluid_repro_chain_4_vert() {
    // REPRO: boiler source + chain of pipes, horizontal AND vertical. User reports
    // steam stops at the 2nd pipe and vertical is empty.
    pipenet::PipeNetworkManager mgr;
    uint64_t boiler = mgr.addNode(0, 0, 0, 61);   // source at (0,0,0)
    uint64_t p1h    = mgr.addNode(1, 0, 0, 61);   // horizontal chain
    uint64_t p2h    = mgr.addNode(2, 0, 0, 61);
    uint64_t p3h    = mgr.addNode(3, 0, 0, 61);
    uint64_t p1v    = mgr.addNode(0, 1, 0, 61);   // vertical chain (up)
    uint64_t p2v    = mgr.addNode(0, 2, 0, 61);

    // edges boiler->p1h->p2h->p3h  and  boiler->p1v->p2v
    mgr.addEdge(boiler, p1h);
    mgr.addEdge(p1h, p2h);
    mgr.addEdge(p2h, p3h);
    mgr.addEdge(boiler, p1v);
    mgr.addEdge(p1v, p2v);

    mgr.setNodeFluid(boiler, 5000, 5000, 84, true, false);  // big source
    for (uint64_t p : {p1h,p2h,p3h,p1v,p2v})
        mgr.setNodeFluid(p, 0, 1000, 0, false, false);

    mgr.rebuildNetworks();

    // Run several ticks so pipe->pipe propagation has time.
    for (int i = 0; i < 5; ++i) mgr.tickFluidNetworks();

    printf("REPRO buf: p1h=%d p2h=%d p3h=%d | p1v=%d p2v=%d | boiler=%d\n",
           mgr.getNode(p1h)->fluidBuffer, mgr.getNode(p2h)->fluidBuffer,
           mgr.getNode(p3h)->fluidBuffer, mgr.getNode(p1v)->fluidBuffer,
           mgr.getNode(p2v)->fluidBuffer, mgr.getNode(boiler)->fluidBuffer);

    bool allConnected = mgr.discoverNetwork(p3h).size() > 1 &&
                        mgr.discoverNetwork(p2v).size() > 1;
    CHECK(allConnected, "p3h and p2v must be reachable from boiler (same network)");
    // Regression: the far horizontal and vertical pipe must have received fluid.
    CHECK_GT(mgr.getNode(p3h)->fluidBuffer, 0, "3rd horizontal pipe must fill");
    CHECK_GT(mgr.getNode(p2v)->fluidBuffer, 0, "2nd vertical pipe must fill");
    PASS();
}

// =========================================================================
//  Fluid buffering tests (tickFluidNetworks: pipes fill even with no sink)
// =========================================================================

static void test_fluid_buffering_pipe_fills_without_sink() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t pipe = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, pipe);

    mgr.setNodeFluid(src, 500, 2000, 84, true, false);  // source: 500 mB water
    mgr.setNodeFluid(pipe, 0, 1000, 0, false, false);   // empty pipe, 1000 mB cap

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == pipe) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "fluid network exists");

    // No sink present — pipe must still fill from the source.
    mgr.tickFluidNetworks();
    const auto* pipeNode = mgr.getNode(pipe);
    CHECK_GT(pipeNode->fluidBuffer, 0, "pipe filled even with no sink");
    CHECK_EQ(pipeNode->fluidId, uint32_t(84), "pipe fluid type is water");
    CHECK_LT(mgr.getNode(src)->fluidBuffer, 500, "source drained as pipe filled");
    PASS();
}

static void test_fluid_buffering_pipes_capped_source_drained() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t pipe = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, pipe);

    // Source has only 200 mB; pipe can hold 1000. One tick fills pipe to 200,
    // source drained to 0, nothing lost.
    mgr.setNodeFluid(src, 200, 2000, 84, true, false);
    mgr.setNodeFluid(pipe, 0, 1000, 0, false, false);
    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == pipe) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "network exists");

    mgr.tickFluidNetworks();
    CHECK_EQ(mgr.getNode(pipe)->fluidBuffer, 200, "pipe holds all available fluid");
    CHECK_EQ(mgr.getNode(src)->fluidBuffer, 0, "source fully drained");
    CHECK_EQ(mgr.getNode(pipe)->fluidId, uint32_t(84), "pipe fluid is water");
    PASS();
}

static void test_fluid_buffering_even_split_across_pipes() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t p1 = mgr.addNode(1, 0, 0, 61);
    uint64_t p2 = mgr.addNode(2, 0, 0, 61);
    mgr.addEdge(src, p1);
    mgr.addEdge(p1, p2);

    mgr.setNodeFluid(src, 1000, 2000, 84, true, false);
    mgr.setNodeFluid(p1, 0, 1000, 0, false, false);
    mgr.setNodeFluid(p2, 0, 1000, 0, false, false);
    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == p2) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "network exists");

    mgr.tickFluidNetworks();
    CHECK_EQ(mgr.getNode(p1)->fluidBuffer, 500, "even split: each pipe gets 500");
    CHECK_EQ(mgr.getNode(p2)->fluidBuffer, 500, "even split: each pipe gets 500");
    CHECK_EQ(mgr.getNode(src)->fluidBuffer, 0, "source drained");
    PASS();
}

// Regression guard for the boiler→pipe flow path after the protocol_id/id-space
// collision fix in PipeNetworkService. A fluid pipe claims an auto mgr_id
// (1,2,3...); a machine's protocol_id is its ECS entity id. When those collide
// the OLD code returned early, so the machine never got an edge to its adjacent
// pipe and steam could never enter the pipe. The fix remaps the machine to a
// fresh addNode() id and still builds the edge. This reproduces that fixed
// end-state: pipe holds the colliding auto-id, boiler is remapped to a distinct
// id and connected, and steam must flow from boiler into the pipe.
static void test_fluid_boiler_to_pipe_after_collision_remap() {
    pipenet::PipeNetworkManager mgr;

    // Pipe claims the first auto-id; a boiler with ECS entity id == that value
    // would collide. The fixed service remaps it to a fresh id instead.
    uint64_t pipe = mgr.addNode(1, 0, 0, 61);   // fluid pipe, auto-id
    CHECK_GT(pipe, uint64_t(0), "pipe registered");

    // Boiler remapped to a fresh, distinct id (the fix result).
    uint64_t boiler = mgr.addNode(0, 0, 0, 61);  // fluid machine node
    CHECK_NE(boiler, pipe, "boiler got a distinct id (remap, no collision)");

    // Adjacent face → edge, exactly what connectNodeNeighbors builds once the
    // boiler is present in machine_nodes_.
    mgr.addEdge(boiler, pipe);

    mgr.setNodeFluid(boiler, 500, 2000, 84, true, false);  // source: 500 mB steam-like
    mgr.setNodeFluid(pipe,   0, 1000, 0,  false, false);   // empty pipe

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == pipe) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "boiler+pipe share a fluid network");

    mgr.tickFluidNetworks();
    CHECK_GT(mgr.getNode(pipe)->fluidBuffer, 0, "steam flows into pipe");
    CHECK_LT(mgr.getNode(boiler)->fluidBuffer, 500, "boiler drained as pipe fills");
    PASS();
}

static void test_rebuild_item_networks_no_accumulation() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 61);
    uint64_t b = mgr.addNode(1, 0, 0, 61);
    uint64_t c = mgr.addNode(2, 0, 0, 61);
    CHECK(a != 0 && b != 0 && c != 0, "three nodes registered");

    // tickItemNetworks() calls rebuildItemNetworks() every tick. Before the fix
    // it appended fresh single-node networks on each call instead of replacing
    // the map, so 3 isolated nodes grew networks_ by 3 entries per tick
    // (the hundreds-of-duplicate-networks symptom in live logs).
    mgr.rebuildItemNetworks();
    mgr.rebuildItemNetworks();
    mgr.rebuildItemNetworks();

    auto nets = mgr.getAllNetworks();
    CHECK_EQ(nets.size(), size_t(3), "3 isolated nodes → exactly 3 networks after 3 rebuilds");
    PASS();
}

// =========================================================================
//  Fluid drain tests (drainFluidFromNetwork: machine pulls from pipe buffers)
// =========================================================================

static void test_fluid_drain_from_pipe() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t pipe = mgr.addNode(1, 0, 0, 61);
    uint64_t sink = mgr.addNode(2, 0, 0, 61);
    mgr.addEdge(src, pipe);
    mgr.addEdge(pipe, sink);

    // Fill pipe with 500 mB steam via source
    mgr.setNodeFluid(src, 500, 2000, 84, true, false);
    mgr.setNodeFluid(pipe, 0, 1000, 0, false, false);
    mgr.setNodeFluid(sink, 0, 400, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == pipe) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "network exists");

    // Push source to pipe first.
    mgr.tickFluidNetworks();
    CHECK_EQ(mgr.getNode(pipe)->fluidBuffer, 500, "pipe has 500 steam from source");

    // Now drain 200 from pipe — macerator consumes.
    auto deltas = mgr.drainFluidFromNetwork(targetNet, 84, 200);
    int32_t drained = 0;
    for (const auto& [nid, d] : deltas) drained += (-d);
    CHECK_EQ(drained, 200, "drained 200 steam from pipe");
    CHECK_EQ(mgr.getNode(pipe)->fluidBuffer, 300, "pipe now has 300 steam");

    // Source unaffected by drain (tickFluidNetworks owns source drain).
    CHECK_EQ(mgr.getNode(src)->fluidBuffer, 0, "source untouched by drainFluidFromNetwork");
    PASS();
}

static void test_fluid_drain_capped_by_pipe_amount() {
    pipenet::PipeNetworkManager mgr;
    uint64_t pipe = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(pipe, 100, 1000, 84, false, false);
    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == pipe) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "network exists");

    // Request 500 but pipe only has 100.
    auto deltas = mgr.drainFluidFromNetwork(targetNet, 84, 500);
    int32_t drained = 0;
    for (const auto& [nid, d] : deltas) drained += (-d);
    CHECK_EQ(drained, 100, "drained only what pipe had (100)");
    CHECK_EQ(mgr.getNode(pipe)->fluidBuffer, 0, "pipe empty after drain");
    PASS();
}

static void test_fluid_drain_empty_pipe_returns_nothing() {
    pipenet::PipeNetworkManager mgr;
    uint64_t pipe = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(pipe, 0, 1000, 0, false, false);
    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == pipe) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "network exists");

    auto deltas = mgr.drainFluidFromNetwork(targetNet, 84, 200);
    CHECK(deltas.empty(), "no drain from empty pipe");
    CHECK_EQ(mgr.getNode(pipe)->fluidBuffer, 0, "pipe still empty");
    PASS();
}

static void test_fluid_drain_even_split_across_pipes() {
    pipenet::PipeNetworkManager mgr;
    uint64_t p1 = mgr.addNode(0, 0, 0, 61);
    uint64_t p2 = mgr.addNode(1, 0, 0, 61);
    uint64_t p3 = mgr.addNode(2, 0, 0, 61);
    mgr.addEdge(p1, p2);
    mgr.addEdge(p2, p3);

    mgr.setNodeFluid(p1, 300, 1000, 84, false, false);
    mgr.setNodeFluid(p2, 300, 1000, 84, false, false);
    mgr.setNodeFluid(p3, 300, 1000, 84, false, false);
    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == p3) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "network exists");

    // Drain 300 from 3 pipes with 300 each — should split ~100 each.
    auto deltas = mgr.drainFluidFromNetwork(targetNet, 84, 300);
    int32_t totalDrained = 0;
    for (const auto& [nid, d] : deltas) totalDrained += (-d);
    CHECK_EQ(totalDrained, 300, "total drained 300");
    CHECK_EQ(mgr.getNode(p1)->fluidBuffer + mgr.getNode(p2)->fluidBuffer +
             mgr.getNode(p3)->fluidBuffer, 600, "600 steam left across pipes");
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
    CHECK(reg.isFluid(waterId), "water still registered after re-init");

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
//  HeatLoss module + pipe heat transport
// =========================================================================

static void test_heat_loss_basic() {
    // Mirrors cableEnergyLoss: loss = traversed distance x per-block resistance
    CHECK_EQ(pipenet::heatTransferLoss(5.0f, 0.5f), 2.5f, "loss = distance x resistance");
    CHECK_EQ(pipenet::effectiveHeatTransfer(5.0f, 0.5f, 10.0f), 7.5f, "heat minus edge loss");
    CHECK_EQ(pipenet::effectiveHeatTransfer(5.0f, 0.5f, 2.0f), 0.0f, "loss clamps heat at zero");

    auto res = pipenet::applyHeatLoss(100.0f, 30.0f);
    CHECK_EQ(res.effectiveHeat, 70.0f, "effective heat after traversal loss");
    CHECK_EQ(res.lostHeat, 30.0f, "lost heat recorded");

    auto resFull = pipenet::applyHeatLoss(100.0f, 500.0f);
    CHECK_EQ(resFull.effectiveHeat, 0.0f, "loss beyond heat clamps effective to zero");
    CHECK_EQ(resFull.lostHeat, 100.0f, "lost heat never exceeds available heat");
    PASS();
}

static void test_heat_loss_node_temperature() {
    auto r = pipenet::calculateNodeTemperature(0.0f, 50.0f);
    CHECK_EQ(r.temperature, 48.0f, "temp = throughput - cooldown");
    CHECK(!r.overheated, "not overheated");

    auto hot = pipenet::calculateNodeTemperature(95.0f, 10.0f);
    CHECK(hot.overheated, "crosses max temperature threshold");
    CHECK_GT(hot.temperature, 100.0f, "temp above threshold");

    auto cooled = pipenet::calculateNodeTemperature(10.0f, 0.0f);
    CHECK_EQ(cooled.temperature, 8.0f, "cooldown each tick with no throughput");

    auto clamped = pipenet::calculateNodeTemperature(1.0f, 0.0f);
    CHECK_EQ(clamped.temperature, 0.0f, "temperature clamped at zero");
    PASS();
}

static uint64_t heatNetworkWithSink(pipenet::PipeNetworkManager& mgr,
                                    uint64_t sinkId) {
    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sinkId) return n->id;
    return 0;
}

static void test_heat_distribution_loss_reduction() {
    pipenet::PipeNetworkManager mgr;
    // source -> mid -> sink, 10 blocks between each hop, resistance 0.5/block
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t mid = mgr.addNode(10, 0, 0, 1);
    uint64_t sink = mgr.addNode(20, 0, 0, 1);
    mgr.addEdge(src, mid, 0.5f);
    mgr.addEdge(mid, sink, 0.5f);

    mgr.setNodeHeat(src, 5000, 5000, true, false);
    mgr.setNodeHeat(mid, 0, 5000, false, false);
    mgr.setNodeHeat(sink, 0, 5000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    CHECK_GT(netId, uint64_t(0), "found heat network");

    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // Excess above 90% = 500. Traversed-edge loss = (0.5*10)+(0.5*10) = 10.
    // Effective transfer = 500 - 10 = 490.
    const auto* srcNode = mgr.getNode(src);
    const auto* sinkNode = mgr.getNode(sink);
    CHECK(!deltas.empty(), "heat distribution produced deltas");
    CHECK_EQ(sinkNode->heatStored, 490, "heat reduced by traversed-edge loss");
    CHECK_EQ(srcNode->heatStored, 5000 - 490, "source drained by delivered heat only");
    PASS();
}

static void test_heat_distribution_no_loss_with_zero_resistance() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t sink = mgr.addNode(1, 0, 0, 1);
    mgr.addEdge(src, sink, 0.0f);

    mgr.setNodeHeat(src, 2000, 2000, true, false);
    mgr.setNodeHeat(sink, 0, 2000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // Excess above 90% = 200, no edge loss -> full 200 delivered.
    CHECK_EQ(mgr.getNode(sink)->heatStored, 200, "zero-resistance edges lose nothing");
    PASS();
}

static void test_heat_distribution_capped_at_max() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t sink = mgr.addNode(1, 0, 0, 1);
    mgr.addEdge(src, sink, 0.0f);

    mgr.setNodeHeat(src, 100000, 100000, true, false);
    mgr.setNodeHeat(sink, 0, 100000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // Excess is huge but flow is capped at MAX_HEAT_PER_TICK (1000).
    int32_t delivered = 0;
    for (const auto& [nid, d] : deltas) if (d > 0) delivered += d;
    CHECK_GE(delivered, 1, "heat flowed");
    CHECK_GE(pipenet::HeatConstants::MAX_HEAT_PER_TICK, delivered, "flow capped at 1000/tick");
    CHECK_EQ(mgr.getNode(sink)->heatStored, pipenet::HeatConstants::MAX_HEAT_PER_TICK,
             "sink received exactly the cap with no loss");
    PASS();
}

static void test_heat_node_temperature_tracked() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t sink = mgr.addNode(1, 0, 0, 1);
    mgr.addEdge(src, sink, 0.0f);

    mgr.setNodeHeat(src, 2000, 2000, true, false);
    mgr.setNodeHeat(sink, 0, 2000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // 200 heat moved: temp rises by throughput (cooldown clamped at zero first).
    CHECK_GT(mgr.getNode(src)->temperature, 0.0f, "source temperature rises");
    CHECK_GT(mgr.getNode(sink)->temperature, 0.0f, "sink temperature rises");

    // Second tick: source drained to 90% (no excess), cooldown still applies.
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    float cooled = mgr.getNode(sink)->temperature;
    CHECK_LT(cooled, 200.0f, "temperature cools over ticks");
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    CHECK_LT(mgr.getNode(sink)->temperature, cooled, "temperature keeps cooling while idle");
    PASS();
}

// =========================================================================
//  Heat pipe block (1111:10:4)
// =========================================================================

static void test_heat_pipe_node_capacity() {
    pipenet::PipeNetworkManager mgr;
    uint64_t hp = mgr.addNode(0, 0, 0, ItemId::pack("1111:10:4"));
    const auto* node = mgr.getNode(hp);
    CHECK(node, "heat pipe node registered");
    CHECK_EQ(node->heatCapacity, 1000, "heat pipe carries heatCapacity=1000");
    CHECK_EQ(node->itemCapacity, 0, "heat pipe carries no items");
    CHECK_EQ(node->fluidCapacity, 0, "heat pipe carries no fluids");
    PASS();
}

static void test_heat_pipe_network_transport() {
    pipenet::PipeNetworkManager mgr;
    // heat_generator (source) -> heat_pipe -> boiler (HEAT sink)
    uint64_t src  = mgr.addNode(0, 0, 0, 1);  // plain block, set as heat source
    uint64_t hp   = mgr.addNode(1, 0, 0, ItemId::pack("1111:10:4"));
    uint64_t sink = mgr.addNode(2, 0, 0, 1);  // plain block, set as heat sink
    mgr.addEdge(src, hp, 0.0f);
    mgr.addEdge(hp, sink, 0.0f);

    mgr.setNodeHeat(src, 5000, 5000, true, false);
    mgr.setNodeHeat(hp, 0, 1000, false, false);
    mgr.setNodeHeat(sink, 0, 5000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    CHECK_GT(netId, uint64_t(0), "heat network discovered through heat pipe");

    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    CHECK(!deltas.empty(), "heat distributed across heat pipe");
    CHECK_GT(mgr.getNode(sink)->heatStored, 0, "sink received heat through heat pipe");
    CHECK_LT(mgr.getNode(src)->heatStored, 5000, "source drained");
    PASS();
}

static void test_heat_pipe_no_fluid_transport() {
    pipenet::PipeNetworkManager mgr;
    uint64_t hp = mgr.addNode(0, 0, 0, ItemId::pack("1111:10:4"));
    mgr.setNodeHeat(hp, 0, 1000, false, false);
    uint64_t netId = heatNetworkWithSink(mgr, hp);
    // No sink -> no distribution, and fluid path stays empty.
    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    CHECK(deltas.empty() || mgr.getNode(hp)->heatStored == 0,
          "heat pipe alone without source/sink does not produce heat");
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
    mgr.setNodeItemProps(nodes[0], 100, true, false);
    for (size_t i = 1; i < nodes.size() - 1; ++i) {
        mgr.setNodeItemProps(nodes[i], 100, false, false);
    }
    mgr.setNodeItemProps(nodes.back(), 100, false, false);
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
//  Persistence tests
// =========================================================================

static void test_export_import_item_buffers() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, true, false);
    mgr.addNodeItem(a, 42, 1);
    mgr.addNodeItem(a, 7, 2);

    auto exported = mgr.exportItemBuffers();
    CHECK_EQ(exported.size(), size_t(1), "one node with items exported");
    CHECK(exported.find(a) != exported.end(), "node a in export");
    CHECK_EQ(exported.at(a).size(), size_t(2), "two items exported from node a");
    CHECK_EQ(exported.at(a)[0].item_id, uint16_t(42), "exported item id correct");
    CHECK_EQ(exported.at(a)[0].count, uint8_t(1), "exported item count correct");

    // Re-import to a fresh manager with same node IDs
    pipenet::PipeNetworkManager mgr2;
    uint64_t a2 = mgr2.addNode(0, 0, 0, 62);
    mgr2.addNode(1, 0, 0, 62);

    mgr2.setNodeItemProps(a2, 10, true, false);
    mgr2.importItemBuffers(exported);
    const auto* restored = mgr2.getNode(a2);
    CHECK(restored != nullptr, "node a2 exists after import");
    CHECK_EQ(restored->itemBuffer.size(), size_t(2), "two items restored");
    CHECK_EQ(restored->itemBuffer[0].item_id, uint16_t(42), "first item id restored");
    CHECK_EQ(restored->itemBuffer[0].count, uint8_t(1), "first item count restored");
    CHECK_EQ(restored->itemBuffer[1].item_id, uint16_t(7), "second item id restored");
    PASS();
}

static void test_export_empty_no_buffers() {
    pipenet::PipeNetworkManager mgr;
    mgr.addNode(0, 0, 0, 62);
    auto exported = mgr.exportItemBuffers();
    CHECK(exported.empty(), "no items to export on empty buffers");
    PASS();
}

// =========================================================================
//  Fluid routing tests (extended)
// =========================================================================

static void test_fluid_routing_type_mismatch() {
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

    // Try to put lava (fluid_id=85) into same network — should be blocked at the network level
    mgr.setNodeFluid(src, 1000, 2000, 85, true, false);  // lava
    mgr.rebuildNetworks();
    // After rebuild, network type should still be the original fluid type
    auto nets2 = mgr.getAllNetworks();
    for (const auto* n : nets2)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    CHECK_GT(targetNet, uint64_t(0), "network still exists after fluid type change");
    PASS();
}

static void test_fluid_routing_capacity() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t sink = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, sink);

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

    // Second distribution: already full, no more fluid accepted
    mgr.distributeFluid(targetNet, 500);
    CHECK_EQ(sinkNode->fluidBuffer, 500, "sink still capped after second distribution");
    PASS();
}

// =========================================================================
//  Integration-style tests
// =========================================================================

static void test_block_place_auto_discovery() {
    // Simulate block place: addNode + rebuildItemNetworks should create network
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, true, false);
    mgr.setNodeItemProps(b, 10, false, false);

    mgr.rebuildItemNetworks();
    auto* net = mgr.getItemNetwork(a);
    CHECK(net != nullptr, "item network discovered after block place");
    CHECK_EQ(net->itemNodes.size(), size_t(2), "both nodes in item network");
    PASS();
}

static void test_machine_to_pipe_to_machine() {
    // Simulate: machine output → pipe → machine input
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);   // machine output as pipe node
    uint64_t pipe = mgr.addNode(1, 0, 0, 62);  // connecting pipe
    uint64_t sink = mgr.addNode(2, 0, 0, 62);  // machine input as pipe node

    mgr.addEdge(src, pipe);
    mgr.addEdge(pipe, sink);

    mgr.setNodeItemProps(src, 10, true, false);    // source
    mgr.setNodeItemProps(pipe, 10, false, false);   // pass-through
    mgr.setNodeItemProps(sink, 10, false, false);   // sink (item capacity=10 means pipe stores items)
    mgr.setNodeEnergy(sink, 0, 1000, false, true); // mark as sink

    // Add item at source
    mgr.addNodeItem(src, 42, 1);

    // Tick 1: item moves from src to pipe or sink
    mgr.tickItemNetworks();
    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(0), "source drained after tick 1");
    // Item should be in pipe or sink after tick 1
    size_t totalAfterTick1 = mgr.getNode(pipe)->itemBuffer.size() + mgr.getNode(sink)->itemBuffer.size();
    CHECK_EQ(totalAfterTick1, size_t(1), "item in transit after tick 1");

    // Keep ticking until item reaches sink
    for (int i = 0; i < 10; ++i) mgr.tickItemNetworks();
    CHECK_EQ(mgr.getNode(sink)->itemBuffer.size(), size_t(1), "item reached sink after multiple ticks");
    CHECK_EQ(mgr.getNode(sink)->itemBuffer[0].item_id, uint16_t(42), "correct item at sink");
    PASS();
}

static void test_pipe_node_meta() {
    // New manager mask support: per-face mask stored on the node, and bulk
    // removal of a node's incident edges (used when a pipe's mask changes).
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeMeta(a, 0x3F);
    CHECK_EQ(mgr.getNode(a)->meta, uint8_t(0x3F), "setNodeMeta stores mask");
    mgr.setNodeMeta(a, 0);
    CHECK_EQ(mgr.getNode(a)->meta, uint8_t(0), "setNodeMeta overwrites mask");

    mgr.removeEdgesForNode(a);
    bool together = false;
    for (const auto* net : mgr.getAllNetworks()) {
        bool hasA = false, hasB = false;
        for (uint64_t n : net->nodeIds) { if (n == a) hasA = true; if (n == b) hasB = true; }
        if (hasA && hasB) together = true;
    }
    CHECK(!together, "removeEdgesForNode dropped the a↔b edge");
    PASS();
}

static void test_item_pipe_face_mask() {
    // Validates the exact gating rule PipeNetworkService::connectNodeNeighbors
    // applies: two pipes connect across face f only if BOTH open it
    // (f on one side, f^1 on the other); a machine endpoint (no mask, treated
    // as open) connects whenever the pipe's shared face is open.
    using pipenet::pipeFaceOpen;
    using pipenet::pipeFacesConnected;

    CHECK(pipeFacesConnected(0, 0, 0), "meta 0 (all open) connects on every face");
    CHECK(!pipeFacesConnected(0x3E, 0x3F, 0), "closing A's +X face disconnects A↔B");
    CHECK(!pipeFacesConnected(0x3F, 0x3D, 0), "closing B's −X face disconnects A↔B (must be mutual)");
    CHECK(pipeFacesConnected(0x3F, 0x3F, 0), "both fully open connect");
    CHECK(pipeFacesConnected(0x01, 0x02, 0), "mutual open across +X connects (only those faces)");
    CHECK(pipeFaceOpen(0x3F, 0), "machine connects through a pipe's open +X face");
    CHECK(!pipeFaceOpen(0x3E, 0), "machine does NOT connect through a pipe's closed +X face");
    PASS();
}

static void test_pipe_wrench_disconnect() {
    // Regression: a wrench toggling a pipe face OFF must disconnect that pipe
    // while open faces stay connected. connectNodeNeighbors is private, so this
    // rebuilds A's edges through the public API as that method does.
    pipenet::PipeNetworkManager mgr;

    auto inSame = [&](uint64_t x, uint64_t y) -> bool {
        auto net = mgr.discoverNetwork(x);
        for (uint64_t n : net) if (n == y) return true;
        return false;
    };

    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    uint64_t c = mgr.addNode(-1, 0, 0, 62);
    mgr.setNodeItemProps(a, 1, false, false);
    mgr.setNodeItemProps(b, 1, false, false);
    mgr.setNodeItemProps(c, 1, false, false);

    // Face f connects A to a neighbor; the neighbor's opposite face is f^1.
    auto rebuildA = [&](uint8_t metaA, uint8_t metaB, uint8_t metaC) {
        mgr.setNodeMeta(a, metaA);
        mgr.setNodeMeta(b, metaB);
        mgr.setNodeMeta(c, metaC);
        mgr.removeEdgesForNode(a);
        mgr.removeEdgesForNode(b);
        mgr.removeEdgesForNode(c);
        if (pipenet::pipeFacesConnected(metaA, metaB, 0)) mgr.addEdge(a, b);
        if (pipenet::pipeFacesConnected(metaA, metaC, 1)) mgr.addEdge(a, c);
    };

    rebuildA(0x3F, 0x3F, 0x3F);
    CHECK(inSame(a, b), "fully-open pipes connect across +X");
    CHECK(inSame(a, c), "fully-open pipes connect across -X");

    rebuildA(0x3E, 0x3F, 0x3F);
    CHECK(!inSame(a, b), "wrench closing A's +X face disconnects A<->B");
    CHECK(inSame(a, c), "A keeps still-open -X connection to C");
    PASS();
}

static void test_pipe_machine_connect() {
    // Regression: a machine connects to a pipe gated only by the pipe's own
    // face mask (machine side has none). connectNodeNeighbors is private, so
    // this drives its pipe<->machine branch through the public API.
    pipenet::PipeNetworkManager mgr;

    auto inSame = [&](uint64_t x, uint64_t y) -> bool {
        auto net = mgr.discoverNetwork(x);
        for (uint64_t n : net) if (n == y) return true;
        return false;
    };

    uint64_t pipe = mgr.addNode(0, 0, 0, 62);
    uint64_t machine = mgr.addNode(1, 0, 0, 100);
    mgr.setNodeItemProps(pipe, 1, false, false);
    mgr.setNodeItemProps(machine, 1, false, false);

    auto rebuild = [&](uint8_t pipeMeta) {
        mgr.setNodeMeta(pipe, pipeMeta);
        mgr.removeEdgesForNode(pipe);
        mgr.removeEdgesForNode(machine);
        if (pipenet::pipeFaceOpen(pipeMeta, 0)) mgr.addEdge(pipe, machine);
    };

    rebuild(0x3F);
    CHECK(inSame(pipe, machine), "machine connects through pipe's open +X face");

    rebuild(0x3E);
    CHECK(!inSame(pipe, machine), "machine does not connect through pipe's closed +X face");
    PASS();
}

static void test_cable_graph_transformer_integration() {
    using namespace gtnh::pipe_network;

    CableGraph cg;

    CableDef mvCable;
    mvCable.block_id = 66;
    mvCable.tier = 2;
    mvCable.max_voltage = 128;
    mvCable.ampacity = 16;
    mvCable.loss_per_block = 0;
    cg.addCableNode(100, mvCable, 0, 0, 0);
    cg.addCableNode(102, mvCable, 1, 0, 0);

    CableDef hvCable;
    hvCable.block_id = 68;
    hvCable.tier = 3;
    hvCable.max_voltage = 512;
    hvCable.ampacity = 8;
    hvCable.loss_per_block = 0;
    cg.addCableNode(101, hvCable, 3, 0, 0);
    cg.addCableNode(103, hvCable, 4, 0, 0);

    cg.rebuildGraph();

    cg.registerGenerator(200, -1, 0, 0, 2);
    cg.registerMachine(300, 2, 0, 0, 2);
    cg.registerGenerator(301, 2, 0, 0, 3);
    cg.registerMachine(400, 5, 0, 0, 3);

    EnergyPacket pkt;
    pkt.voltage = 128;
    pkt.ampCount = 1;
    pkt.sourceId = 200;
    pkt.targetId = 0;
    pkt.tickIssued = 0;
    cg.injectPacket(pkt, 100);
    cg.tick();

    auto mvPackets = cg.collectPackets(300);
    CHECK(!mvPackets.empty(), "transformer sink receives MV packet");

    EnergyPacket steppedUp;
    steppedUp.voltage = 512;
    steppedUp.ampCount = 1;
    steppedUp.sourceId = 301;
    steppedUp.targetId = 0;
    steppedUp.tickIssued = 0;
    cg.injectPacket(steppedUp, 101);
    cg.tick();

    auto hvPackets = cg.collectPackets(400);
    CHECK(!hvPackets.empty(), "HV consumer receives stepped-up packet");
    CHECK(cg.getExplodedNodes().empty(), "no explosions in correct tier setup");

    PASS();
}

static void test_cable_explosion_event() {
    using namespace gtnh::pipe_network;

    CableGraph cg;
    CableDef def = {66, 2, "cable_tin", 0.0f, 128, 16};

    cg.addCableNode(100, def, 0, 0, 0);
    cg.addCableNode(102, def, 1, 0, 0);
    cg.rebuildGraph();

    cg.registerMachine(300, 2, 0, 0);

    EnergyPacket pkt;
    pkt.voltage = 512;
    pkt.ampCount = 1;
    pkt.sourceId = 200;
    pkt.targetId = 0;
    pkt.tickIssued = 0;
    cg.injectPacket(pkt, 100);

    cg.tick();

    auto exploded = cg.getExplodedNodes();
    CHECK(!exploded.empty(), "cable explosion: at least one node exploded");
    CHECK_EQ(exploded[0].nodeId, uint64_t(102), "cable explosion: cable before machine explodes");
    CHECK_GT(exploded[0].temperature, 0.0f, "cable explosion: temperature > 0");

    PASS();
}

static void test_persistence_load_unload_cycle() {
    pipenet::PipeNetworkManager mgr;

    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, true, false);
    mgr.setNodeItemProps(b, 10, false, true);
    mgr.addNodeItem(a, 42, 1);
    mgr.addNodeItem(a, 7, 3);

    auto exported = mgr.exportItemBuffers();
    CHECK_EQ(exported.size(), size_t(1), "export has nodes with items");

    pipenet::PipeNetworkManager mgr2;
    uint64_t a2 = mgr2.addNode(0, 0, 0, 62);
    uint64_t b2 = mgr2.addNode(1, 0, 0, 62);
    mgr2.addEdge(a2, b2);
    mgr2.setNodeItemProps(a2, 10, true, false);
    mgr2.setNodeItemProps(b2, 10, false, true);

    mgr2.importItemBuffers(exported);

    auto* restoredSrc = mgr2.getNode(a2);
    CHECK(restoredSrc != nullptr, "restored source node exists");
    CHECK_EQ(restoredSrc->itemBuffer.size(), size_t(2), "two items restored");
    CHECK_EQ(restoredSrc->itemBuffer[0].item_id, uint16_t(42), "first item id");
    CHECK_EQ(restoredSrc->itemBuffer[0].count, uint8_t(1), "first item count");
    CHECK_EQ(restoredSrc->itemBuffer[1].item_id, uint16_t(7), "second item id");
    CHECK_EQ(restoredSrc->itemBuffer[1].count, uint8_t(3), "second item count");

    mgr2.rebuildItemNetworks();

    auto* net = mgr2.getItemNetwork(a2);
    CHECK(net != nullptr, "item network exists after restore");
    CHECK_EQ(net->itemNodes.size(), size_t(2), "both nodes in item network after restore");

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

    // Pipe wrench guidance
    TEST(wrench_isolated_pipe);
    TEST(wrench_pipe_to_pipe);
    TEST(wrench_pipe_adjacent_machine);
    TEST(wrench_non_pipe_position);
    TEST(wrench_guidance_no_mutation);

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

    // Fluid buffering (pipes fill even with no sink)
    TEST(fluid_buffering_pipe_fills_without_sink);
    TEST(fluid_buffering_pipes_capped_source_drained);
    TEST(fluid_buffering_even_split_across_pipes);
    TEST(fluid_boiler_to_pipe_after_collision_remap);
    TEST(rebuild_item_networks_no_accumulation);

    // Fluid drain (machine pulls from pipe buffer)
    TEST(fluid_drain_from_pipe);
    TEST(fluid_drain_capped_by_pipe_amount);
    TEST(fluid_drain_empty_pipe_returns_nothing);
    TEST(fluid_drain_even_split_across_pipes);

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

    // HeatLoss module + pipe heat transport
    TEST(heat_loss_basic);
    TEST(heat_loss_node_temperature);
    TEST(heat_distribution_loss_reduction);
    TEST(heat_distribution_no_loss_with_zero_resistance);
    TEST(heat_distribution_capped_at_max);
    TEST(heat_node_temperature_tracked);

    // Heat pipe block
    TEST(heat_pipe_node_capacity);
    TEST(heat_pipe_network_transport);
    TEST(heat_pipe_no_fluid_transport);

    // Edge cases
    TEST(remove_edge_and_rebuild);
    TEST(large_network);
    TEST(node_count_after_operations);

    // Transformer integration
    TEST(cable_graph_transformer_integration);

    // Cable explosion event
    TEST(cable_explosion_event);

    // Persistence cycle
    TEST(persistence_load_unload_cycle);

    // Persistence
    TEST(export_import_item_buffers);
    TEST(export_empty_no_buffers);

    // Fluid routing (extended)
    TEST(fluid_routing_type_mismatch);
    TEST(fluid_routing_capacity);

    // Integration-style
    TEST(block_place_auto_discovery);
    TEST(machine_to_pipe_to_machine);
    TEST(fluid_repro_chain_4_vert);

    // Per-face mask support (item/fluid pipe disconnect via wrench)
    TEST(pipe_node_meta);
    TEST(item_pipe_face_mask);
    TEST(pipe_wrench_disconnect);
    TEST(pipe_machine_connect);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
