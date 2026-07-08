#pragma once
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pipenet {

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

  // BFS: discover connected networks from a node
  std::vector<uint64_t> discoverNetwork(uint64_t startNodeId) const;

  // Recalculate all networks (BFS from unvisited nodes)
  void rebuildNetworks();

  // Distribute energy across a network for one tick
  // Returns map of node_id -> energy_delta (positive = received, negative =
  // sent)
  std::unordered_map<uint64_t, int32_t> distributeEnergy(uint64_t networkId,
                                                         int32_t tickEnergy);

  // Distribute fluid across a network for one tick
  std::unordered_map<uint64_t, int32_t> distributeFluid(uint64_t networkId,
                                                        int32_t tickFluid);

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
                        bool isItemSource);
  void addNodeItem(uint64_t nodeId, uint16_t itemId, uint8_t count);

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

  // BFS helper
  void bfsNetwork(uint64_t startNode, std::unordered_set<uint64_t> &visited,
                  std::vector<uint64_t> &component);

  // Distribute flow evenly across nodes
  void distributeFlow(std::vector<uint64_t> &nodeIds, int32_t totalAmount,
                      std::unordered_map<uint64_t, int32_t> &deltas);
};

} // namespace pipenet
