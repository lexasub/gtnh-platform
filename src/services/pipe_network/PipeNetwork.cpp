#include "PipeNetwork.h"
#include "HeatLoss.h"
#include "PipeBlockIds.h"
#include <queue>
#include <algorithm>
#include <cstdlib>
#include <cassert>
#include <spdlog/spdlog.h>

namespace pipenet {

WrenchGuidance evaluatePipeWrench(
    const std::unordered_map<uint64_t, uint64_t>& pipe_nodes,
    const std::unordered_map<uint64_t, uint64_t>& machine_nodes,
    int32_t x, int32_t y, int32_t z, uint64_t* out_node_id) {
    if (out_node_id) *out_node_id = 0;

    auto it = pipe_nodes.find(pipePosKey(x, y, z));
    if (it == pipe_nodes.end()) return WrenchGuidance::NOT_A_PIPE;
    if (out_node_id) *out_node_id = it->second;

    static const int dx[6] = {0, 0, 0, 0, -1, 1};
    static const int dy[6] = {-1, 1, 0, 0, 0, 0};
    static const int dz[6] = {0, 0, -1, 1, 0, 0};

    bool pipe_neighbor = false;
    bool machine_neighbor = false;
    for (int f = 0; f < 6; ++f) {
        uint64_t adj = pipePosKey(x + dx[f], y + dy[f], z + dz[f]);
        if (pipe_nodes.count(adj) > 0) pipe_neighbor = true;
        else if (machine_nodes.count(adj) > 0) machine_neighbor = true;
    }

    if (machine_neighbor) return WrenchGuidance::CONNECTED;
    if (pipe_neighbor) return WrenchGuidance::CONNECT_TO_MACHINE;
    return WrenchGuidance::CONNECT_PIPES;
}

PipeNetworkManager::PipeNetworkManager() = default;
PipeNetworkManager::~PipeNetworkManager() = default;

uint64_t PipeNetworkManager::addNode(int32_t x, int32_t y, int32_t z, uint16_t blockId) {
    uint64_t id = nextNodeId_++;
    PipeNode node{};
    node.id = id;
    node.x = x;
    node.y = y;
    node.z = z;
    node.block_id = blockId;
    node.energyBuffer = 0;
    node.energyCapacity = 0;
    node.fluidBuffer = 0;
    node.fluidCapacity = 0;
    node.fluidId = 0;
    node.itemBuffer.clear();
    node.itemCapacity = 0;
    node.isItemSource = false;
    node.heatStored = 0;
    node.heatCapacity = 0;
    node.isSource = false;
    node.isSink = false;
    node.isItemSink = false;

    switch (blockId) {
        case BLOCK_ID_ITEM_PIPE:
            node.itemCapacity = 4;
            node.fluidCapacity = 0;
            break;
        case BLOCK_ID_DENSE_ITEM_PIPE:
            node.itemCapacity = 16;
            node.fluidCapacity = 0;
            break;
        case BLOCK_ID_FLUID_PIPE:
            node.itemCapacity = 0;
            node.fluidCapacity = 1000;
            break;
        case BLOCK_ID_DENSE_FLUID_PIPE:
            node.itemCapacity = 0;
            node.fluidCapacity = 4000;
            break;
        default:
            break;
    }

    nodes_[id] = node;
    rebuildNetworks();
    return id;
}

bool PipeNetworkManager::addNodeWithId(uint64_t id, int32_t x, int32_t y, int32_t z, uint16_t blockId) {
    if (nodes_.find(id) != nodes_.end()) return false;
    PipeNode node{};
    node.id = id;
    node.x = x;
    node.y = y;
    node.z = z;
    node.block_id = blockId;
    node.energyBuffer = 0;
    node.energyCapacity = 0;
    node.fluidBuffer = 0;
    node.fluidCapacity = 0;
    node.fluidId = 0;
    node.itemBuffer.clear();
    node.itemCapacity = 0;
    node.isItemSource = false;
    node.heatStored = 0;
    node.heatCapacity = 0;
    node.isSource = false;
    node.isSink = false;
    node.isItemSink = false;

    switch (blockId) {
        case BLOCK_ID_ITEM_PIPE:
            node.itemCapacity = 4;
            node.fluidCapacity = 0;
            break;
        case BLOCK_ID_DENSE_ITEM_PIPE:
            node.itemCapacity = 16;
            node.fluidCapacity = 0;
            break;
        case BLOCK_ID_FLUID_PIPE:
            node.itemCapacity = 0;
            node.fluidCapacity = 1000;
            break;
        case BLOCK_ID_DENSE_FLUID_PIPE:
            node.itemCapacity = 0;
            node.fluidCapacity = 4000;
            break;
        default:
            break;
    }

    nodes_[id] = node;
    rebuildNetworks();
    return true;
}

void PipeNetworkManager::removeNode(uint64_t nodeId) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    for (auto ei = edges_.begin(); ei != edges_.end(); ) {
        if (ei->second.fromNode == nodeId || ei->second.toNode == nodeId) {
            ei = edges_.erase(ei);
        } else {
            ++ei;
        }
    }
    nodes_.erase(it);
    rebuildNetworks();
}

uint64_t PipeNetworkManager::addEdge(uint64_t fromNode, uint64_t toNode, float resistance) {
    if (nodes_.find(fromNode) == nodes_.end() || nodes_.find(toNode) == nodes_.end()) {
        return 0;
    }
    for (const auto& [eid, edge] : edges_) {
        if ((edge.fromNode == fromNode && edge.toNode == toNode) ||
            (edge.fromNode == toNode && edge.toNode == fromNode)) {
            return eid;
        }
    }
    uint64_t id = nextEdgeId_++;
    InternalEdge edge{id, fromNode, toNode, resistance};
    edges_[id] = edge;
    rebuildNetworks();
    return id;
}

void PipeNetworkManager::removeEdge(uint64_t edgeId) {
    edges_.erase(edgeId);
    rebuildNetworks();
}

void PipeNetworkManager::bfsNetwork(uint64_t startNode, std::unordered_set<uint64_t>& visited,
                                     std::vector<uint64_t>& component) {
    std::queue<uint64_t> q;
    q.push(startNode);
    visited.insert(startNode);

    std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
    for (const auto& [eid, edge] : edges_) {
        adjacency[edge.fromNode].push_back(edge.toNode);
        adjacency[edge.toNode].push_back(edge.fromNode);
    }

    while (!q.empty()) {
        uint64_t current = q.front();
        q.pop();
        component.push_back(current);

        auto ai = adjacency.find(current);
        if (ai == adjacency.end()) continue;

        for (uint64_t neighbor : ai->second) {
            if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                q.push(neighbor);
            }
        }
    }
}

std::vector<uint64_t> PipeNetworkManager::discoverNetwork(uint64_t startNodeId) const {
    std::vector<uint64_t> component;
    if (nodes_.find(startNodeId) == nodes_.end()) return component;

    std::unordered_set<uint64_t> visited;
    std::queue<uint64_t> q;
    q.push(startNodeId);
    visited.insert(startNodeId);

    std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
    for (const auto& [eid, edge] : edges_) {
        adjacency[edge.fromNode].push_back(edge.toNode);
        adjacency[edge.toNode].push_back(edge.fromNode);
    }

    while (!q.empty()) {
        uint64_t current = q.front();
        q.pop();
        component.push_back(current);

        auto ai = adjacency.find(current);
        if (ai == adjacency.end()) continue;

        for (uint64_t neighbor : ai->second) {
            if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                q.push(neighbor);
            }
        }
    }
    return component;
}

void PipeNetworkManager::rebuildNetworks() {
    nodeToNetwork_.clear();
    networks_.clear();

    std::unordered_set<uint64_t> visited;

    for (const auto& [nid, node] : nodes_) {
        if (visited.find(nid) != visited.end()) continue;

        std::vector<uint64_t> component;
        bfsNetwork(nid, visited, component);

        if (component.empty()) continue;

        uint64_t netId = nextNetworkId_++;
        PipeNetwork net{};
        net.id = netId;
        net.nodeIds = component;
        net.totalEnergy = 0;
        net.totalFluid = 0;
        net.fluidId = 0;
        net.isActive = false;

        uint32_t firstFluidId = 0;
        bool fluidMixed = false;
        for (uint64_t cnid : component) {
            auto ni = nodes_.find(cnid);
            if (ni == nodes_.end()) continue;
            net.totalEnergy += ni->second.energyBuffer;
            net.totalFluid += ni->second.fluidBuffer;
            if (ni->second.fluidId != 0) {
                if (firstFluidId == 0) {
                    firstFluidId = ni->second.fluidId;
                } else if (ni->second.fluidId != firstFluidId) {
                    fluidMixed = true;
                }
            }
            nodeToNetwork_[cnid] = netId;
        }
        net.fluidId = fluidMixed ? 0 : firstFluidId;
        networks_[netId] = net;
    }
}

[[maybe_unused]] static bool isSourceNode(const PipeNode& node) {
    return node.isSource;
}

[[maybe_unused]] static bool isSinkNode(const PipeNode& node) {
    return node.isSink;
}

void PipeNetworkManager::distributeFlow(std::vector<uint64_t>& nodeIds, int32_t totalAmount,
                                        std::unordered_map<uint64_t, int32_t>& deltas) {
    if (nodeIds.empty() || totalAmount == 0) return;

    std::vector<uint64_t> sources;
    std::vector<uint64_t> sinks;
    for (uint64_t nid : nodeIds) {
        auto ni = nodes_.find(nid);
        if (ni == nodes_.end()) continue;
        if (ni->second.isSource) sources.push_back(nid);
        if (ni->second.isSink) sinks.push_back(nid);
    }

    // Remove energy from sources proportionally to their capacity
    int32_t totalSourceCapacity = 0;
    for (uint64_t sid : sources) {
        auto ni = nodes_.find(sid);
        if (ni != nodes_.end()) {
            totalSourceCapacity += std::max(1, ni->second.energyCapacity);
        }
    }

    if (!sources.empty() && totalSourceCapacity > 0) {
        int32_t remaining = totalAmount;
        for (size_t i = 0; i < sources.size(); ++i) {
            uint64_t sid = sources[i];
            auto ni = nodes_.find(sid);
            if (ni == nodes_.end()) continue;
            int32_t take;
            if (i == sources.size() - 1) {
                take = remaining;
            } else {
                take = static_cast<int32_t>(
                    static_cast<int64_t>(totalAmount) *
                    std::max(1, ni->second.energyCapacity) / totalSourceCapacity);
            }
            take = std::min(take, ni->second.energyBuffer);
            deltas[sid] -= take;
            ni->second.energyBuffer -= take;
            remaining -= take;
        }
    }

    if (!sinks.empty()) {
        int32_t perSink = totalAmount / static_cast<int32_t>(sinks.size());
        int32_t remainder = totalAmount % static_cast<int32_t>(sinks.size());
        for (size_t i = 0; i < sinks.size(); ++i) {
            uint64_t snid = sinks[i];
            auto ni = nodes_.find(snid);
            if (ni == nodes_.end()) continue;
            int32_t give = perSink + (i < static_cast<size_t>(remainder) ? 1 : 0);
            int32_t room = ni->second.energyCapacity - ni->second.energyBuffer;
            give = std::min(give, room);
            deltas[snid] += give;
            ni->second.energyBuffer += give;
        }
    }
}

std::unordered_map<uint64_t, int32_t> PipeNetworkManager::distributeEnergy(uint64_t networkId, int32_t tickEnergy) {
    std::unordered_map<uint64_t, int32_t> deltas;
    auto ni = networks_.find(networkId);
    if (ni == networks_.end() || tickEnergy == 0) return deltas;

    PipeNetwork& net = ni->second;
    distributeFlow(net.nodeIds, tickEnergy, deltas);

    net.totalEnergy = 0;
    bool anySink = false;
    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;
        net.totalEnergy += nodeIt->second.energyBuffer;
        if (nodeIt->second.isSink) anySink = true;
    }
    net.isActive = anySink && tickEnergy != 0;

    return deltas;
}

std::unordered_map<uint64_t, int32_t> PipeNetworkManager::distributeFluid(uint64_t networkId, int32_t tickFluid) {
    std::unordered_map<uint64_t, int32_t> deltas;
    auto ni = networks_.find(networkId);
    if (ni == networks_.end() || tickFluid == 0) return deltas;

    PipeNetwork& net = ni->second;

    // For fluid: only distribute to empty sinks (fluidId == 0 or same fluid)
    std::vector<uint64_t> sinks;
    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;
        bool isSink = nodeIt->second.isSink;
        bool hasRoom = nodeIt->second.fluidBuffer < nodeIt->second.fluidCapacity;
        bool fluidOk = nodeIt->second.fluidId == 0;
        if (isSink && hasRoom && fluidOk) {
            sinks.push_back(nid);
        }
    }

    if (!sinks.empty()) {
        int32_t perSink = tickFluid / static_cast<int32_t>(sinks.size());
        int32_t remainder = tickFluid % static_cast<int32_t>(sinks.size());
        for (size_t i = 0; i < sinks.size(); ++i) {
            uint64_t snid = sinks[i];
            auto nodeIt = nodes_.find(snid);
            if (nodeIt == nodes_.end()) continue;
            int32_t give = perSink + (i < static_cast<size_t>(remainder) ? 1 : 0);
            int32_t room = nodeIt->second.fluidCapacity - nodeIt->second.fluidBuffer;
            give = std::min(give, room);
            deltas[snid] += give;
            nodeIt->second.fluidBuffer += give;
            if (nodeIt->second.fluidId == 0 && give > 0) {
                nodeIt->second.fluidId = net.fluidId != 0 ? net.fluidId : 1;
            }
        }
    }

    net.totalFluid = 0;
    bool anySink = false;
    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;
        net.totalFluid += nodeIt->second.fluidBuffer;
        if (nodeIt->second.isSink) anySink = true;
    }
    net.isActive = anySink && tickFluid != 0;

    return deltas;
}

const PipeNode* PipeNetworkManager::getNode(uint64_t nodeId) const {
    auto it = nodes_.find(nodeId);
    return it != nodes_.end() ? &it->second : nullptr;
}

const PipeNetwork* PipeNetworkManager::getNetwork(uint64_t networkId) const {
    auto it = networks_.find(networkId);
    return it != networks_.end() ? &it->second : nullptr;
}

std::vector<const PipeNetwork*> PipeNetworkManager::getAllNetworks() const {
    std::vector<const PipeNetwork*> result;
    result.reserve(networks_.size());
    for (const auto& [nid, net] : networks_) {
        result.push_back(&net);
    }
    return result;
}

PipeNetwork* PipeNetworkManager::getItemNetwork(uint64_t nodeId) {
    auto ni = nodeToNetwork_.find(nodeId);
    if (ni == nodeToNetwork_.end()) return nullptr;
    uint64_t netId = ni->second;
    auto netIt = networks_.find(netId);
    return netIt != networks_.end() ? &netIt->second : nullptr;
}

void PipeNetworkManager::rebuildItemNetworks() {
    std::unordered_set<uint64_t> visited;
    for (const auto& [nid, node] : nodes_) {
        if (visited.find(nid) != visited.end()) continue;

        std::vector<uint64_t> component;
        bfsNetwork(nid, visited, component);

        if (component.empty()) continue;

        uint64_t netId = nextNetworkId_++;
        PipeNetwork net{};
        net.id = netId;
        net.nodeIds = component;
        net.totalEnergy = 0;
        net.totalFluid = 0;
        net.fluidId = 0;
        net.isActive = false;

        // Filter for item nodes (nodes with item capacity)
        for (uint64_t cnid : component) {
            auto ni = nodes_.find(cnid);
            if (ni == nodes_.end()) continue;
            net.totalEnergy += ni->second.energyBuffer;
            net.totalFluid += ni->second.fluidBuffer;

            // Check if node has item capacity (is an item pipe)
            if (ni->second.itemCapacity > 0) {
                net.itemNodes.push_back(cnid);
                if (ni->second.isItemSource) {
                    net.itemTransferRate = std::max(net.itemTransferRate, 1.0f);
                }
            }
        }

        // Store the network and update nodeToNetwork mapping
        networks_[netId] = net;
        for (uint64_t cnid : component) {
            nodeToNetwork_[cnid] = netId;
        }
    }
}

uint64_t PipeNetworkManager::findNextItemHop(uint64_t currentNodeId, uint64_t networkId) {
    auto netIt = networks_.find(networkId);
    if (netIt == networks_.end()) return 0;

    // Build adjacency for the network's item nodes
    std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
    for (const auto& [eid, edge] : edges_) {
        adjacency[edge.fromNode].push_back(edge.toNode);
        adjacency[edge.toNode].push_back(edge.fromNode);
    }

    // BFS from currentNodeId to find the next connected item-capable node
    std::unordered_set<uint64_t> visited;
    std::queue<uint64_t> q;
    q.push(currentNodeId);
    visited.insert(currentNodeId);

    while (!q.empty()) {
        uint64_t current = q.front();
        q.pop();

        auto ai = adjacency.find(current);
        if (ai == adjacency.end()) continue;

        for (uint64_t neighbor : ai->second) {
            if (visited.find(neighbor) != visited.end()) continue;
            visited.insert(neighbor);

            if (neighbor == currentNodeId) continue;

            auto nodeIt = nodes_.find(neighbor);
            if (nodeIt == nodes_.end()) continue;

            // Found an item-capable node
            if (nodeIt->second.itemCapacity > 0) {
                return neighbor;
            }

            // If neighbor is a sink (machine), return it directly
            if (nodeIt->second.isSink) {
                return neighbor;
            }

            q.push(neighbor);
        }
    }

    return 0;
}

std::vector<ConsumedItemEvent> PipeNetworkManager::moveItemsInNetwork(uint64_t networkId) {
    std::vector<ConsumedItemEvent> consumed;
    auto netIt = networks_.find(networkId);
    if (netIt == networks_.end()) return consumed;

    PipeNetwork& net = netIt->second;
    if (net.itemNodes.empty()) return consumed;

    // Build adjacency for path finding
    std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
    for (const auto& [eid, edge] : edges_) {
        adjacency[edge.fromNode].push_back(edge.toNode);
        adjacency[edge.toNode].push_back(edge.fromNode);
    }

    // Collect sources that have items and sinks that have room
    std::vector<uint64_t> sources;
    std::vector<uint64_t> sinks;
    for (uint64_t nid : net.itemNodes) {
        auto ni = nodes_.find(nid);
        if (ni == nodes_.end()) continue;

        // Source: marked as item source and has items to send
        if (ni->second.isItemSource && !ni->second.itemBuffer.empty()) {
            sources.push_back(nid);
        }

        // Sink: marked as sink and has room in inventory
                if (ni->second.isSink || ni->second.isItemSink) {
                    // Check if this sink has inventory room (using ItemSlot as capacity indicator)
                    // For raw pipes acting as sinks, check itemBuffer vs itemCapacity
                    if (ni->second.itemBuffer.size() < static_cast<size_t>(ni->second.itemCapacity) ||
                        ni->second.itemCapacity == 0) {
                        sinks.push_back(nid);
                    }
                }
    }

    if (sources.empty() || sinks.empty()) return consumed;

    // Move one item from each source to the nearest sink
    for (uint64_t srcId : sources) {
        if (sinks.empty()) break;

        auto srcIt = nodes_.find(srcId);
        if (srcIt == nodes_.end() || srcIt->second.itemBuffer.empty()) continue;

        // BFS from source to find nearest sink
        std::unordered_set<uint64_t> visited;
        std::queue<uint64_t> q;
        q.push(srcId);
        visited.insert(srcId);

        uint64_t bestSink = 0;
        std::unordered_map<uint64_t, uint64_t> parent;

        while (!q.empty() && bestSink == 0) {
            uint64_t current = q.front();
            q.pop();

            auto ai = adjacency.find(current);
            if (ai == adjacency.end()) continue;

            for (uint64_t neighbor : ai->second) {
                if (visited.find(neighbor) != visited.end()) continue;
                visited.insert(neighbor);
                parent[neighbor] = current;

                // Check if this neighbor is a desired sink
                auto ni = nodes_.find(neighbor);
                if (ni == nodes_.end()) continue;

                if (ni->second.isSink || ni->second.isItemSink) {
                    // Verify the sink has room
                    bool hasRoom = ni->second.itemBuffer.size() <
                                   static_cast<size_t>(ni->second.itemCapacity) ||
                                   ni->second.itemCapacity == 0;
                    if (hasRoom) {
                        bestSink = neighbor;
                        break;
                    }
                }

                // Only continue through item-capable pipes
                if (ni->second.itemCapacity > 0) {
                    q.push(neighbor);
                }
            }
        }

        if (bestSink == 0) continue;

        // Move one ItemSlot from source to sink
        ItemSlot moving = srcIt->second.itemBuffer.back();
        srcIt->second.itemBuffer.pop_back();

        // Put item into sink's buffer
        auto sinkIt = nodes_.find(bestSink);
        if (sinkIt != nodes_.end()) {
            if (sinkIt->second.itemCapacity > 0 &&
                sinkIt->second.itemBuffer.size() < static_cast<size_t>(sinkIt->second.itemCapacity)) {
                sinkIt->second.itemBuffer.push_back(moving);
            } else {
                // Item consumed at machine sink (capacity == 0)
                ConsumedItemEvent ev;
                ev.sinkNodeId = bestSink;
                ev.sourceNodeId = srcId;
                ev.item = moving;
                ev.x = sinkIt->second.x;
                ev.y = sinkIt->second.y;
                ev.z = sinkIt->second.z;
                consumed.push_back(ev);
            }
            // If sink has no item capacity, item is "consumed" by the machine
        }
    }
    return consumed;
}

void PipeNetworkManager::tickItemNetworks() {
    consumedItemEvents_.clear();
    rebuildItemNetworks();

    for (auto& [netId, net] : networks_) {
        if (net.itemNodes.empty()) continue;
        auto evs = moveItemsInNetwork(netId);
        consumedItemEvents_.insert(consumedItemEvents_.end(), evs.begin(), evs.end());
    }
}

const std::vector<ConsumedItemEvent>& PipeNetworkManager::getConsumedItemEvents() const {
    return consumedItemEvents_;
}

void PipeNetworkManager::setNodeEnergy(uint64_t nodeId, int32_t energy, int32_t capacity,
                                        bool isSource, bool isSink) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.energyBuffer = energy;
    it->second.energyCapacity = capacity;
    it->second.isSource = isSource;
    it->second.isSink = isSink;
}

void PipeNetworkManager::setNodeFluid(uint64_t nodeId, int32_t fluid, int32_t capacity,
                                       uint32_t fluidId, bool isSource, bool isSink) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.fluidBuffer = fluid;
    it->second.fluidCapacity = capacity;
    it->second.fluidId = fluidId;
    it->second.isSource = isSource;
    it->second.isSink = isSink;
}

void PipeNetworkManager::setNodeItemProps(uint64_t nodeId, uint8_t itemCapacity, bool isItemSource, bool isItemSink) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.itemCapacity = itemCapacity;
    it->second.isItemSource = isItemSource;
    it->second.isItemSink = isItemSink;
    it->second.isSink = isItemSink;
}

void PipeNetworkManager::addNodeItem(uint64_t nodeId, uint16_t itemId, uint8_t count) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.itemBuffer.push_back({itemId, count});
}

void PipeNetworkManager::setNodeHeat(uint64_t nodeId, int32_t heat, int32_t capacity,
                                     bool isSource, bool isSink) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.heatStored = heat;
    it->second.heatCapacity = capacity;
    it->second.isSource = isSource;
    it->second.isSink = isSink;
}

void PipeNetworkManager::setNodeSideConfig(uint64_t nodeId,
                                           const std::array<uint8_t, 6>& sideConfig) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.side_config = sideConfig;
}

float PipeNetworkManager::computeNetworkHeatLoss(uint64_t networkId) const {
    float totalLoss = 0.0f;
    for (const auto& [eid, edge] : edges_) {
        auto fromIt = nodeToNetwork_.find(edge.fromNode);
        if (fromIt == nodeToNetwork_.end() || fromIt->second != networkId) continue;
        auto fn = nodes_.find(edge.fromNode);
        auto tn = nodes_.find(edge.toNode);
        if (fn == nodes_.end() || tn == nodes_.end()) continue;
        int32_t distance = std::abs(fn->second.x - tn->second.x) +
                           std::abs(fn->second.y - tn->second.y) +
                           std::abs(fn->second.z - tn->second.z);
        totalLoss += edge.resistance * static_cast<float>(distance);
    }
    return totalLoss;
}

std::unordered_map<uint64_t, int32_t> PipeNetworkManager::distributeHeat(uint64_t networkId,
                                                                          int32_t tickHeat) {
    std::unordered_map<uint64_t, int32_t> deltas;
    auto ni = networks_.find(networkId);
    if (ni == networks_.end() || tickHeat == 0) return deltas;

    PipeNetwork& net = ni->second;

    // Per-node temperature cools every tick regardless of flow (HeatLoss).
    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;
        nodeIt->second.temperature =
            calculateNodeTemperature(nodeIt->second.temperature, 0.0f).temperature;
    }

    std::vector<uint64_t> heatSources;
    std::vector<uint64_t> heatSinks;

    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;

        const auto& node = nodeIt->second;

        if (node.isSource && node.heatStored > node.heatCapacity * 0.9) {
            heatSources.push_back(nid);
        }

        if (node.isSink && node.heatStored < node.heatCapacity) {
            heatSinks.push_back(nid);
        }
    }

    if (heatSources.empty() || heatSinks.empty()) return deltas;
    
    int32_t totalExcessHeat = 0;
    for (uint64_t sourceId : heatSources) {
        auto nodeIt = nodes_.find(sourceId);
        if (nodeIt == nodes_.end()) continue;
        const auto& node = nodeIt->second;
        int32_t excess = node.heatStored - static_cast<int32_t>(node.heatCapacity * 0.9);
        totalExcessHeat += excess;
    }
    
    if (totalExcessHeat <= 0) return deltas;
    
    int32_t heatToFlow = std::min(tickHeat, totalExcessHeat);
    heatToFlow = std::min(heatToFlow, HeatConstants::MAX_HEAT_PER_TICK);

    if (heatToFlow <= 0) return deltas;

    // Effective transfer reduced by traversed-edge resistance x distance.
    // Heat dissipated along the path never reaches the sinks (HeatLoss).
    HeatLossResult loss = applyHeatLoss(static_cast<float>(heatToFlow),
                                        computeNetworkHeatLoss(networkId));
    heatToFlow = static_cast<int32_t>(loss.effectiveHeat);
    if (heatToFlow <= 0) return deltas;

    std::vector<std::pair<uint64_t, int32_t>> sortedSinks;
    for (uint64_t sinkId : heatSinks) {
        auto nodeIt = nodes_.find(sinkId);
        if (nodeIt == nodes_.end()) continue;
        sortedSinks.push_back({sinkId, nodeIt->second.heatStored});
    }
    
    std::sort(sortedSinks.begin(), sortedSinks.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    
    int32_t remainingHeat = heatToFlow;
    for (auto& sinkPair : sortedSinks) {
        if (remainingHeat <= 0) break;
        
        uint64_t sinkId = sinkPair.first;
        auto nodeIt = nodes_.find(sinkId);
        if (nodeIt == nodes_.end()) continue;
        
        auto& node = nodeIt->second;
        
        int32_t room = node.heatCapacity - node.heatStored;
        if (room <= 0) continue;
        
        int32_t give = std::min(remainingHeat, room);
        
        for (uint64_t sourceId : heatSources) {
            if (give <= 0) break;
            
            auto sourceIt = nodes_.find(sourceId);
            if (sourceIt == nodes_.end()) continue;
            
            auto& sourceNode = sourceIt->second;
            
            int32_t currentExcess = sourceNode.heatStored - static_cast<int32_t>(sourceNode.heatCapacity * 0.9);
            if (currentExcess <= 0) continue;
            
            int32_t take = std::min(give, currentExcess);
            sourceNode.heatStored -= take;
            node.heatStored += take;
            
            deltas[sourceId] -= take;
            deltas[sinkId] += take;
            
            remainingHeat -= take;
            give -= take;
            
            spdlog::debug("[PipeNet] heat transfer: node {} -> {} ({},{},{}) heat transferred: {}",
                          sourceId, sinkId, node.x, node.y, node.z, take);
        }
    }

    // Temperature rises with heat moved through the node (|delta|). Cooldown
    // was already applied above so cooldown is not double-counted.
    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;
        auto dit = deltas.find(nid);
        if (dit == deltas.end()) continue;
        nodeIt->second.temperature +=
            static_cast<float>(std::abs(dit->second)) * TEMPERATURE_PER_HEAT;
    }

    net.totalEnergy = 0;
    bool anySink = false;
    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;
        net.totalEnergy += nodeIt->second.heatStored;
        if (nodeIt->second.isSink) anySink = true;
    }
    net.isActive = anySink && tickHeat != 0;

    return deltas;
}

std::unordered_map<uint64_t, std::vector<ItemSlot>> PipeNetworkManager::exportItemBuffers() const {
    std::unordered_map<uint64_t, std::vector<ItemSlot>> result;
    for (const auto& [nid, node] : nodes_) {
        if (!node.itemBuffer.empty()) {
            result[nid] = node.itemBuffer;
        }
    }
    return result;
}

void PipeNetworkManager::importItemBuffers(const std::unordered_map<uint64_t, std::vector<ItemSlot>>& buffers) {
    for (const auto& [nid, items] : buffers) {
        auto it = nodes_.find(nid);
        if (it == nodes_.end()) continue;
        it->second.itemBuffer = items;
    }
}

} // namespace pipenet
