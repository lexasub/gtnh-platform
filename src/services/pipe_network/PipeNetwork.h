#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pipenet {

namespace HeatConstants {
constexpr int32_t MAX_HEAT_PER_TICK = 1000; // Maximum heat flow per tick
} // namespace HeatConstants

// Position key for world coords → single uint64 map key (same layout as
// PipeNetworkService::posKey; shared so evaluator and service agree).
inline uint64_t pipePosKey(int32_t x, int32_t y, int32_t z) {
    return (static_cast<uint64_t>(static_cast<int64_t>(x)) << 42)
         | (static_cast<uint64_t>(static_cast<int64_t>(y) & 0xFFFFF) << 20)
         | (static_cast<uint64_t>(static_cast<int64_t>(z) & 0xFFFFF));
}

// Per-face connection mask helpers. A pipe's `meta` byte encodes which of its 6
// faces are open: bit f (0=+X,1=-X,2=+Y,3=-Y,4=+Z,5=-Z) set ⇒ face open.
// meta == 0 ⇒ all faces open (0x3F). Two pipes connect across a shared face f
// only if BOTH open it (f on one side, opposite f^1 on the other).
inline bool pipeFaceOpen(uint8_t meta, int face) {
    return meta == 0 || ((meta & (1u << face)) != 0);
}
inline bool pipeFacesConnected(uint8_t fromMeta, uint8_t toMeta, int face) {
    return pipeFaceOpen(fromMeta, face) && pipeFaceOpen(toMeta, face ^ 1);
}

// Wrench-on-pipe guidance (mirrors Protocol::PipeWrenchGuidance; kept local so
// the evaluator stays testable without FlatBuffers headers).
enum class WrenchGuidance : uint8_t {
    NOT_A_PIPE = 0,
    CONNECT_PIPES = 1,
    CONNECT_TO_MACHINE = 2,
    CONNECTED = 3,
};

// Pure connectivity evaluation for a wrench event — no graph mutation.
//   pipe_nodes:    pos_key → node id (registered pipe blocks)
//   machine_nodes: pos_key → node id (registered machine nodes)
// Fills *out_node_id with the pipe node at (x,y,z) when present (0 otherwise).
WrenchGuidance evaluatePipeWrench(
    const std::unordered_map<uint64_t, uint64_t>& pipe_nodes,
    const std::unordered_map<uint64_t, uint64_t>& machine_nodes,
    int32_t x, int32_t y, int32_t z, uint64_t* out_node_id);

struct ItemSlot {
  uint16_t item_id;
  uint8_t count;
};

struct ConsumedItemEvent {
  uint64_t sinkNodeId;
  uint64_t sourceNodeId;
  ItemSlot item;
  int32_t x, y, z;  // world position of the sink
};

struct PipeNode {
  uint64_t id;
  int32_t x, y, z;
  uint16_t block_id;
  uint8_t meta = 0;       // per-face connection mask; 0 ⇒ all faces open (0x3F)

  // Energy handling
  int32_t energyBuffer;   // current energy stored in this node
  int32_t energyCapacity; // max energy this node can hold

  // Fluid handling
  int32_t fluidBuffer;   // current fluid (mB)
  int32_t fluidCapacity; // max fluid capacity (mB)
  uint32_t fluidId;      // 0 = empty/no fluid

  // Item handling
  std::vector<ItemSlot> itemBuffer;
  uint8_t itemCapacity = 0;
  bool isItemSource = false;
  bool isItemSink = false;

  // Heat handling
  int32_t heatStored;    // current heat stored in this node
  int32_t heatCapacity;  // max heat this node can hold
  float temperature = 0.0f; // per-node temperature, tracked by HeatLoss module

  // Side config for machine sink routing
  std::array<uint8_t, 6> side_config;

  bool isSource; // generator/input
  bool isSink;   // consumer/output
};

struct PipeEdge {
  uint64_t fromNode;
  uint64_t toNode;
  float resistance; // energy loss factor (0.0 = perfect, 1.0 = total loss)
};

struct PipeNetwork {
  uint64_t id;
  std::vector<uint64_t> nodeIds;
  int32_t totalEnergy; // total EU in network
  int32_t totalFluid;  // total fluid in network (mB)
  uint32_t fluidId;    // fluid type in network (0 = mixed/empty)
  bool isActive;       // has flow this tick

  // Item network
  std::vector<uint64_t> itemNodes;
  float itemTransferRate = 1.0f;
};

class PipeNetworkManager {
public:
  PipeNetworkManager();
  ~PipeNetworkManager();

  // Add/remove node
  uint64_t addNode(int32_t x, int32_t y, int32_t z, uint16_t blockId);
  // Add node with explicit ID (returns false if ID already exists)
  bool addNodeWithId(uint64_t id, int32_t x, int32_t y, int32_t z,
                     uint16_t blockId);
  void removeNode(uint64_t nodeId);

  // Add/remove connection between nodes
  uint64_t addEdge(uint64_t fromNode, uint64_t toNode, float resistance = 0.0f);
  void removeEdge(uint64_t edgeId);

  // Per-face connection mask for item/fluid pipe nodes.
  void setNodeMeta(uint64_t nodeId, uint8_t meta);
  // Drop all incident edges so they can be recomputed from the current mask.
  void removeEdgesForNode(uint64_t nodeId);

  // BFS: discover connected networks from a node
  std::vector<uint64_t> discoverNetwork(uint64_t startNodeId) const;

  // Recalculate all networks (BFS from unvisited nodes)
  void rebuildNetworks();

  // Recalculate networks only if a topology mutation happened since the last
  // rebuild. Deferred rebuild keeps the connected-component view consistent
  // (no fragmented intermediate states observed by ticks or consume requests).
  void rebuildNetworksIfDirty() {
    if (networksDirty_) rebuildNetworks();
  }

  // Distribute energy across a network for one tick
  // Returns map of node_id -> energy_delta (positive = received, negative =
  // sent)
  std::unordered_map<uint64_t, int32_t> distributeEnergy(uint64_t networkId,
                                                         int32_t tickEnergy);

  // Distribute fluid across a network for one tick
  std::unordered_map<uint64_t, int32_t> distributeFluid(uint64_t networkId,
                                                         int32_t tickFluid);

  // Per-tick fluid buffering: push source fluid into every node with capacity.
  // Pipes buffer fluid even with no downstream sink (GTNH-style fill). Returns
  // node_id -> fluid_delta (positive = received, negative = drained from a
  // source). Source fluid is read from PipeNode::fluidBuffer, so callers must
  // sync the live machine-source amount into the graph before invoking.
  std::unordered_map<uint64_t, int32_t> tickFluidNetworks();

  // Drain fluid from pipe nodes (non-source, non-sink) in a network for machine
  // consumption. Scans pipe nodes with matching fluidId, drains proportionally,
  // returns node_id -> delta (negative = drained). Callers should check at least
  // one drain occurred before publishing a response.
  std::unordered_map<uint64_t, int32_t> drainFluidFromNetwork(uint64_t networkId,
                                                              uint32_t fluidId,
                                                              int32_t amount);

  // Item network operations
  void rebuildItemNetworks();
  std::vector<ConsumedItemEvent> moveItemsInNetwork(uint64_t networkId);
  void tickItemNetworks();
  const std::vector<ConsumedItemEvent>& getConsumedItemEvents() const;
  uint64_t findNextItemHop(uint64_t currentNodeId, uint64_t networkId);
  PipeNetwork *getItemNetwork(uint64_t nodeId);

  // Node property setters (used by PipeNetworkService and tests)
  void setNodeEnergy(uint64_t nodeId, int32_t energy, int32_t capacity,
                     bool isSource, bool isSink);
  void setNodeFluid(uint64_t nodeId, int32_t fluid, int32_t capacity,
                    uint32_t fluidId, bool isSource, bool isSink);
  void setNodeItemProps(uint64_t nodeId, uint8_t itemCapacity,
                         bool isItemSource, bool isItemSink);
  void addNodeItem(uint64_t nodeId, uint16_t itemId, uint8_t count);
  void setNodeHeat(uint64_t nodeId, int32_t heat, int32_t capacity,
                   bool isSource, bool isSink);
  void setNodeSideConfig(uint64_t nodeId, const std::array<uint8_t, 6>& sideConfig);

  std::unordered_map<uint64_t, int32_t> distributeHeat(uint64_t networkId, int32_t tickHeat);

  std::unordered_map<uint64_t, std::vector<ItemSlot>> exportItemBuffers() const;
  void importItemBuffers(const std::unordered_map<uint64_t, std::vector<ItemSlot>>& buffers);

  // Query
  const PipeNode *getNode(uint64_t nodeId) const;
  const PipeNetwork *getNetwork(uint64_t networkId) const;
  std::vector<const PipeNetwork *> getAllNetworks() const;
  size_t nodeCount() const { return nodes_.size(); }
  size_t networkCount() const { return networks_.size(); }
  size_t edgeCount() const { return edges_.size(); }

private:
  struct InternalEdge {
    uint64_t id;
    uint64_t fromNode;
    uint64_t toNode;
    float resistance;
  };

  std::unordered_map<uint64_t, PipeNode> nodes_;
  std::unordered_map<uint64_t, InternalEdge> edges_;
  std::unordered_map<uint64_t, uint64_t>
      nodeToNetwork_; // node_id -> network_id
  std::unordered_map<uint64_t, PipeNetwork> networks_;

  std::vector<ConsumedItemEvent> consumedItemEvents_;

  uint64_t nextNodeId_{1};
  uint64_t nextEdgeId_{1};
  uint64_t nextNetworkId_{1};

  // Set by any topology mutation; rebuildNetworks() is deferred to the next
  // tick (lazy) so a placement that touches N edges produces ONE rebuild of the
  // final connected graph instead of N fragmented intermediate ones. This is
  // what keeps tickFluidNetworks() from seeing a torn/split network mid-build.
  bool networksDirty_ = true;

  // BFS helper
  void bfsNetwork(uint64_t startNode, std::unordered_set<uint64_t> &visited,
                  std::vector<uint64_t> &component);

  // Distribute flow evenly across nodes
  void distributeFlow(std::vector<uint64_t> &nodeIds, int32_t totalAmount,
                      std::unordered_map<uint64_t, int32_t> &deltas);

  // Total traversed-edge heat loss for a network: sum of resistance x distance
  // over every edge in the network (consulted by distributeHeat via HeatLoss).
  float computeNetworkHeatLoss(uint64_t networkId) const;
};

} // namespace pipenet
