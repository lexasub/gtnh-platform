#include "PipeNetwork.h"
#include "HeatLoss.h"
#include "PipeBlockIds.h"
#include <queue>
#include <algorithm>
#include <cstdlib>
#include <cassert>
#include <spdlog/spdlog.h>

namespace pipenet {

namespace {
constexpr size_t kFluidDomainIdx = static_cast<size_t>(gtnh::common::ResourceKind::FLUID);
constexpr size_t kEuDomainIdx = static_cast<size_t>(gtnh::common::ResourceKind::EU);
constexpr size_t kHuDomainIdx = static_cast<size_t>(gtnh::common::ResourceKind::HU);
constexpr size_t kItemDomainIdx = static_cast<size_t>(gtnh::common::ResourceKind::ITEM);
} // namespace

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

bool PipeNetworkManager::registerPort(const gtnh::common::ResourcePort& port) {
    // owner_id == 0 is a valid owner (tests cover entity ID zero and
    // manager/EnTT ID collisions); only the port identity must be non-zero.
    if (!port.valid()) return false;

    PortKey key{port.owner_id, port.resource_kind, port.port_id};
    auto it = ports_.find(key);
    if (it == ports_.end()) {
        ports_.emplace(key, port);
    } else {
        // Replaying an epoch is an update-in-place: it cannot create a duplicate,
        // while retries with identical data remain observationally idempotent.
        // Epochs are monotonic so delayed updates cannot overwrite current state.
        if (port.epoch < it->second.epoch) return false;
        it->second = port;
    }

    // 2.3.2: route the typed registration into its resource domain so solvers
    // apply role/rate policy at solve time. A port is never consumable through
    // another resource kind (see consumeFluidViaPort / warnCrossKindConsume).
    uint64_t node = findNodeAtPosition(port.x, port.y, port.z);
    projectNodeDomain(node, port.resource_kind);
    spdlog::debug(
        "[PipeNet] port {} owner {} kind {} role {} -> {} at ({},{},{})",
        port.port_id, port.owner_id, static_cast<int>(port.resource_kind),
        static_cast<int>(port.role),
        node != 0 ? "node domain" : "registry only (no node at position)",
        port.x, port.y, port.z);
    return true;
}

bool PipeNetworkManager::removePort(uint64_t ownerId,
                                    gtnh::common::ResourceKind resourceKind,
                                    gtnh::common::PortId portId,
                                    uint64_t epoch) {
    if (portId == 0) return false;

    PortKey key{ownerId, resourceKind, portId};
    auto it = ports_.find(key);
    if (it == ports_.end() || it->second.epoch != epoch) return false;
    const int32_t px = it->second.x;
    const int32_t py = it->second.y;
    const int32_t pz = it->second.z;
    ports_.erase(it);
    projectNodeDomain(findNodeAtPosition(px, py, pz), resourceKind);
    return true;
}

bool PipeNetworkManager::removePort(uint64_t ownerId,
                                    gtnh::common::ResourceKind resourceKind,
                                    gtnh::common::PortId portId) {
    if (portId == 0) return false;

    PortKey key{ownerId, resourceKind, portId};
    auto it = ports_.find(key);
    if (it == ports_.end()) return false;
    const int32_t px = it->second.x;
    const int32_t py = it->second.y;
    const int32_t pz = it->second.z;
    ports_.erase(it);
    projectNodeDomain(findNodeAtPosition(px, py, pz), resourceKind);
    return true;
}

size_t PipeNetworkManager::removePortsForOwner(uint64_t ownerId) {
    struct AffectedPort {
        int32_t x, y, z;
        gtnh::common::ResourceKind kind;
    };
    std::vector<AffectedPort> affected;
    size_t removed = 0;
    for (auto it = ports_.begin(); it != ports_.end();) {
        if (it->first.owner_id == ownerId) {
            affected.push_back({it->second.x, it->second.y, it->second.z,
                                it->first.resource_kind});
            it = ports_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    for (const auto& port : affected) {
        projectNodeDomain(findNodeAtPosition(port.x, port.y, port.z), port.kind);
    }
    return removed;
}

const gtnh::common::ResourcePort* PipeNetworkManager::getPort(
    uint64_t ownerId, gtnh::common::ResourceKind resourceKind,
    gtnh::common::PortId portId) const {
    PortKey key{ownerId, resourceKind, portId};
    auto it = ports_.find(key);
    return it == ports_.end() ? nullptr : &it->second;
}

bool PipeNetworkManager::hasPort(uint64_t ownerId,
                                 gtnh::common::ResourceKind resourceKind,
                                 gtnh::common::PortId portId) const {
    return getPort(ownerId, resourceKind, portId) != nullptr;
}

std::vector<gtnh::common::ResourcePort>
PipeNetworkManager::getRegisteredPorts() const {
    std::vector<gtnh::common::ResourcePort> result;
    result.reserve(ports_.size());
    for (const auto& [key, port] : ports_) result.push_back(port);
    return result;
}

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
    node.heatStored = 0;
    node.heatCapacity = 0;

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
        case BLOCK_ID_HEAT_PIPE:
            node.itemCapacity = 0;
            node.fluidCapacity = 0;
            node.heatCapacity = 1000;
            break;
        default:
            break;
    }

    nodes_[id] = node;
    indexNodePosition(id, x, y, z);
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
    node.heatStored = 0;
    node.heatCapacity = 0;

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
        case BLOCK_ID_HEAT_PIPE:
            node.itemCapacity = 0;
            node.fluidCapacity = 0;
            node.heatCapacity = 1000;
            break;
        default:
            break;
    }

    nodes_[id] = node;
    indexNodePosition(id, x, y, z);
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
    const int32_t x = it->second.x;
    const int32_t y = it->second.y;
    const int32_t z = it->second.z;
    auto pos = node_by_pos_.find(pipePosKey(x, y, z));
    if (pos != node_by_pos_.end() && pos->second == nodeId) {
        node_by_pos_.erase(pos);
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

size_t PipeNetworkManager::addEdges(
    const std::vector<std::pair<uint64_t, uint64_t>>& nodePairs,
    float resistance) {
    size_t added = 0;
    for (const auto& [fromNode, toNode] : nodePairs) {
        if (nodes_.find(fromNode) == nodes_.end() ||
            nodes_.find(toNode) == nodes_.end()) {
            continue;
        }
        bool exists = false;
        for (const auto& [eid, edge] : edges_) {
            if ((edge.fromNode == fromNode && edge.toNode == toNode) ||
                (edge.fromNode == toNode && edge.toNode == fromNode)) {
                exists = true;
                break;
            }
        }
        if (exists) continue;
        const uint64_t id = nextEdgeId_++;
        edges_[id] = InternalEdge{id, fromNode, toNode, resistance};
        ++added;
    }
    if (added != 0) rebuildNetworks();
    return added;
}

void PipeNetworkManager::removeEdge(uint64_t edgeId) {
    edges_.erase(edgeId);
    rebuildNetworks();
}

void PipeNetworkManager::setNodeMeta(uint64_t nodeId, uint8_t meta) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.meta = meta;
}

void PipeNetworkManager::removeEdgesForNode(uint64_t nodeId) {
    for (auto it = edges_.begin(); it != edges_.end();) {
        if (it->second.fromNode == nodeId || it->second.toNode == nodeId) {
            it = edges_.erase(it);
        } else {
            ++it;
        }
    }
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

void PipeNetworkManager::distributeFlow(std::vector<uint64_t>& nodeIds, int32_t totalAmount,
                                        std::unordered_map<uint64_t, int32_t>& deltas) {
    if (nodeIds.empty() || totalAmount == 0) return;

    std::vector<uint64_t> sources;
    std::vector<uint64_t> sinks;
    for (uint64_t nid : nodeIds) {
        auto ni = nodes_.find(nid);
        if (ni == nodes_.end()) continue;
        if (ni->second.domains[kEuDomainIdx].is_source) sources.push_back(nid);
        if (ni->second.domains[kEuDomainIdx].is_sink) sinks.push_back(nid);
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
            if (const auto& domain = ni->second.domains[kEuDomainIdx]; domain.rate > 0) {
                take = std::min(take, domain.rate);
            }
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
            if (const auto& domain = ni->second.domains[kEuDomainIdx]; domain.rate > 0) {
                give = std::min(give, domain.rate);
            }
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
        if (nodeIt->second.domains[kEuDomainIdx].is_sink) anySink = true;
    }
    net.isActive = anySink && tickEnergy != 0;

    return deltas;
}

FluidTransferResult PipeNetworkManager::consumeFluidUncached(uint64_t nodeId,
                                                               uint64_t requestId,
                                                               uint32_t fluidId,
                                                               int32_t amount) {
    FluidTransferResult result{requestId, nodeId, fluidId, 0, 0, false};
    if (amount <= 0 || fluidId == 0) {
        result.blocked = true;
        result.remaining = amount > 0 ? amount : 0;
        return result;
    }

    auto it = nodes_.find(nodeId);
    if (it == nodes_.end() || it->second.fluidCapacity <= 0 ||
        !it->second.domains[kFluidDomainIdx].is_sink) {
        if (it != nodes_.end()) warnCrossKindConsume(it->second);
        result.blocked = true;
        result.remaining = amount;
        return result;
    }

    auto& node = it->second;
    if (node.fluidId != 0 && node.fluidId != fluidId) {
        result.blocked = true;
        result.remaining = amount;
        return result;
    }

    result.accepted_amount = (std::min)(amount, node.fluidBuffer);
    if (const auto& domain = node.domains[kFluidDomainIdx]; domain.rate > 0) {
        result.accepted_amount = std::min(result.accepted_amount, domain.rate);
    }
    node.fluidBuffer -= result.accepted_amount;
    result.remaining = amount - result.accepted_amount;
    if (node.fluidBuffer == 0) node.fluidId = 0;
    return result;
}

FluidTransferResult PipeNetworkManager::consumeFluid(uint64_t nodeId,
                                                      uint64_t requestId,
                                                      uint32_t fluidId,
                                                      int32_t amount) {
    ++fluid_tick_;
    expireFluidTransactions(fluid_tick_);
    if (requestId != 0) {
        if (auto it = fluid_transactions_.find(requestId);
            it != fluid_transactions_.end()) {
            if (it->second.result.node_id != nodeId ||
                it->second.result.fluid_id != fluidId ||
                it->second.requested_amount != amount) {
                FluidTransferResult conflict{requestId, nodeId, fluidId, 0,
                                             amount > 0 ? amount : 0, true};
                return conflict;
            }
            return it->second.result;
        }
    }

    auto result = consumeFluidUncached(nodeId, requestId, fluidId, amount);
    if (requestId != 0) {
        fluid_transactions_.emplace(
            requestId, FluidTransaction{result, amount, fluid_tick_ + kFluidTransactionTtl});
    }
    return result;
}

int32_t PipeNetworkManager::fluidAmount(uint64_t nodeId, uint32_t fluidId) const {
    const auto it = nodes_.find(nodeId);
    if (it == nodes_.end() ||
        (it->second.fluidId != 0 && it->second.fluidId != fluidId)) {
        return 0;
    }
    return it->second.fluidBuffer;
}

void PipeNetworkManager::expireFluidTransactions(uint64_t nowTick) {
    for (auto it = fluid_transactions_.begin(); it != fluid_transactions_.end();) {
        if (it->second.expires_at <= nowTick) {
            it = fluid_transactions_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t PipeNetworkManager::fluidTransactionCount() const {
    return fluid_transactions_.size();
}

std::unordered_map<uint64_t, int32_t> PipeNetworkManager::distributeFluid(uint64_t networkId, int32_t tickFluid) {
    ++fluid_tick_;
    expireFluidTransactions(fluid_tick_);

    // Legacy distribution remains available for existing callers. Consume
    // transactions are handled by consumeFluid() and never mirror owner state.

    std::unordered_map<uint64_t, int32_t> deltas;
    auto ni = networks_.find(networkId);
    if (ni == networks_.end() || tickFluid == 0) return deltas;

    PipeNetwork& net = ni->second;

    // For fluid: only distribute to empty sinks (fluidId == 0 or same fluid)
    std::vector<uint64_t> sinks;
    for (uint64_t nid : net.nodeIds) {
        auto nodeIt = nodes_.find(nid);
        if (nodeIt == nodes_.end()) continue;
        bool isSink = nodeIt->second.domains[kFluidDomainIdx].is_sink;
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
            if (const auto& domain = nodeIt->second.domains[kFluidDomainIdx]; domain.rate > 0) {
                give = std::min(give, domain.rate);
            }
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
        if (nodeIt->second.domains[kFluidDomainIdx].is_sink) anySink = true;
    }
    net.isActive = anySink && tickFluid != 0;

    return deltas;
}

std::unordered_map<uint64_t, int32_t>
PipeNetworkManager::fillFluidPipesFromSources(uint64_t networkId) {
    std::unordered_map<uint64_t, int32_t> deltas;
    auto networkIt = networks_.find(networkId);
    if (networkIt == networks_.end()) return deltas;

    PipeNetwork& net = networkIt->second;
    std::vector<uint64_t> sources;
    std::vector<uint64_t> pipes;
    int64_t available = 0;
    int64_t room = 0;
    uint32_t fluidId = 0;

    for (uint64_t nodeId : net.nodeIds) {
        auto nodeIt = nodes_.find(nodeId);
        if (nodeIt == nodes_.end()) continue;
        const PipeNode& node = nodeIt->second;
        const auto& fluidDomain = node.domains[kFluidDomainIdx];
        if (fluidDomain.is_source && node.fluidBuffer > 0 &&
            node.fluidCapacity > 0 && node.fluidId != 0) {
            sources.push_back(nodeId);
            available += node.fluidBuffer;
            if (fluidId == 0) fluidId = node.fluidId;
            else if (fluidId != node.fluidId) fluidId = 0;
        }
        if (node.fluidCapacity > 0 && !fluidDomain.is_source &&
            !fluidDomain.is_sink) {
            const int32_t nodeRoom = node.fluidCapacity - node.fluidBuffer;
            if (nodeRoom > 0) {
                pipes.push_back(nodeId);
                room += nodeRoom;
            }
        }
    }

    // A mixed source network must not inject an ambiguous fluid into a pipe.
    if (sources.empty() || pipes.empty() || available <= 0 || room <= 0 ||
        fluidId == 0) {
        return deltas;
    }

    const int32_t toFill = static_cast<int32_t>(
        std::min<int64_t>(available, room));
    int32_t remaining = toFill;
    for (size_t i = 0; i < pipes.size(); ++i) {
        auto nodeIt = nodes_.find(pipes[i]);
        if (nodeIt == nodes_.end()) continue;
        const int32_t nodeRoom = nodeIt->second.fluidCapacity -
                                  nodeIt->second.fluidBuffer;
        int32_t give = (i + 1 == pipes.size())
                           ? remaining
                           : static_cast<int32_t>(
                                 (static_cast<int64_t>(toFill) * nodeRoom) / room);
        give = std::min(give, nodeRoom);
        if (give <= 0) continue;
        nodeIt->second.fluidBuffer += give;
        if (nodeIt->second.fluidId == 0) nodeIt->second.fluidId = fluidId;
        deltas[pipes[i]] += give;
        remaining -= give;
    }

    // Move exactly the amount accepted by pipe capacity out of the source
    // mirrors. This is what makes a second tick a no-op until the owner emits a
    // fresh source update; copying without this debit would duplicate fluid.
    int32_t sourceRemaining = toFill;
    int64_t sourceAvailable = available;
    for (size_t i = 0; i < sources.size() && sourceRemaining > 0; ++i) {
        auto nodeIt = nodes_.find(sources[i]);
        if (nodeIt == nodes_.end()) continue;
        const int32_t sourceBuffer = nodeIt->second.fluidBuffer;
        int32_t take = (i + 1 == sources.size())
                           ? sourceRemaining
                           : static_cast<int32_t>(
                                 (static_cast<int64_t>(toFill) * sourceBuffer) /
                                 sourceAvailable);
        take = std::min(take, sourceBuffer);
        if (take <= 0) continue;
        nodeIt->second.fluidBuffer -= take;
        deltas[sources[i]] -= take;
        sourceRemaining -= take;
        sourceAvailable -= sourceBuffer;
    }

    net.totalFluid = 0;
    net.fluidId = fluidId;
    for (uint64_t nodeId : net.nodeIds) {
        auto nodeIt = nodes_.find(nodeId);
        if (nodeIt != nodes_.end()) net.totalFluid += nodeIt->second.fluidBuffer;
    }
    net.isActive = !deltas.empty();
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
    // This is a full topology rebuild, not an incremental append. Keeping the
    // previous networks here leaves stale components in getAllNetworks() and
    // makes each block update grow the work performed by every tick.
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

        // Filter for item nodes (nodes with item capacity)
        for (uint64_t cnid : component) {
            auto ni = nodes_.find(cnid);
            if (ni == nodes_.end()) continue;
            net.totalEnergy += ni->second.energyBuffer;
            net.totalFluid += ni->second.fluidBuffer;

            // Check if node has item capacity (is an item pipe)
            if (ni->second.itemCapacity > 0) {
                net.itemNodes.push_back(cnid);
                if (ni->second.domains[kItemDomainIdx].is_source) {
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
            if (nodeIt->second.domains[kItemDomainIdx].is_sink) {
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
        if (ni->second.domains[kItemDomainIdx].is_source && !ni->second.itemBuffer.empty()) {
            sources.push_back(nid);
        }

        // Sink: marked as sink and has room in inventory
                if (ni->second.domains[kItemDomainIdx].is_sink) {
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

                if (ni->second.domains[kItemDomainIdx].is_sink) {
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

// LEGACY NODE-UPDATE ADAPTER (2.3.4) — see the removal-point note above the
// setNode* declarations in PipeNetwork.h. Each setter routes its legacy
// is_source/is_sink pair into exactly one resource domain.

void PipeNetworkManager::setNodeEnergy(uint64_t nodeId, int32_t energy, int32_t capacity,
                                        bool isSource, bool isSink) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.energyBuffer = energy;
    it->second.energyCapacity = capacity;
    auto& domain = it->second.domains[kEuDomainIdx];
    domain.is_source = isSource;
    domain.is_sink = isSink;
}

void PipeNetworkManager::setNodeFluid(uint64_t nodeId, int32_t fluid, int32_t capacity,
                                       uint32_t fluidId, bool isSource, bool isSink) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.fluidBuffer = fluid;
    it->second.fluidCapacity = capacity;
    it->second.fluidId = fluidId;
    auto& domain = it->second.domains[kFluidDomainIdx];
    domain.is_source = isSource;
    domain.is_sink = isSink;
}

void PipeNetworkManager::setNodeItemProps(uint64_t nodeId, uint8_t itemCapacity, bool isItemSource, bool isItemSink) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    it->second.itemCapacity = itemCapacity;
    auto& domain = it->second.domains[kItemDomainIdx];
    domain.is_source = isItemSource;
    domain.is_sink = isItemSink;
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
    auto& domain = it->second.domains[kHuDomainIdx];
    domain.is_source = isSource;
    domain.is_sink = isSink;
}

const DomainNodeState* PipeNetworkManager::nodeDomain(
    uint64_t nodeId, gtnh::common::ResourceKind kind) const {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return nullptr;
    return &it->second.domains[domainIndex(kind)];
}

uint64_t PipeNetworkManager::findNodeAtPosition(int32_t x, int32_t y, int32_t z) const {
    auto it = node_by_pos_.find(pipePosKey(x, y, z));
    return it == node_by_pos_.end() ? 0 : it->second;
}

void PipeNetworkManager::indexNodePosition(uint64_t nodeId, int32_t x, int32_t y, int32_t z) {
    node_by_pos_[pipePosKey(x, y, z)] = nodeId;
    for (size_t i = 0; i < kResourceDomainCount; ++i) {
        projectNodeDomain(nodeId, static_cast<gtnh::common::ResourceKind>(i));
    }
}

void PipeNetworkManager::projectNodeDomain(uint64_t nodeId,
                                           gtnh::common::ResourceKind kind) {
    auto it = nodes_.find(nodeId);
    if (it == nodes_.end()) return;
    const auto& node = it->second;

    // Domain state for `kind` is derived from the typed ports of that kind at
    // the node position: roles OR together (a machine may expose a source and
    // a sink in one domain), rate/face policy take the most permissive value.
    // No matching ports clears the domain, so a removed port stops receiving
    // flow immediately.
    DomainNodeState state;
    for (const auto& [key, port] : ports_) {
        if (key.resource_kind != kind) continue;
        if (port.x != node.x || port.y != node.y || port.z != node.z) continue;
        state.is_source = state.is_source || port.role == gtnh::common::PortRole::SOURCE;
        state.is_sink = state.is_sink || port.role == gtnh::common::PortRole::SINK;
        state.rate = std::max(state.rate, port.rate);
        state.face_mask = static_cast<uint8_t>(state.face_mask | port.face_mask);
    }
    it->second.domains[domainIndex(kind)] = state;
}

void PipeNetworkManager::warnCrossKindConsume(const PipeNode& node) const {
    int other_kind_ports = 0;
    for (const auto& [key, port] : ports_) {
        if (key.resource_kind == gtnh::common::ResourceKind::FLUID) continue;
        if (port.x == node.x && port.y == node.y && port.z == node.z) {
            ++other_kind_ports;
        }
    }
    if (other_kind_ports > 0) {
        spdlog::warn(
            "[PipeNet] fluid consume rejected at node {} ({},{},{}): {} registered "
            "port(s) at this position belong to other resource domains",
            node.id, node.x, node.y, node.z, other_kind_ports);
    }
}

FluidTransferResult PipeNetworkManager::consumeFluidViaPort(
    uint64_t ownerId, gtnh::common::PortId portId,
    uint64_t requestId, uint32_t fluidId, int32_t amount) {
    FluidTransferResult blocked{requestId, 0, fluidId, 0,
                                amount > 0 ? amount : 0, true};

    const auto* port = getPort(ownerId, gtnh::common::ResourceKind::FLUID, portId);
    if (!port) {
        // 2.3.2 enforcement: the port id may exist under another resource
        // kind; consuming fluid through it is rejected and logged.
        for (const auto& [key, p] : ports_) {
            if (key.owner_id == ownerId && key.port_id == portId) {
                spdlog::warn(
                    "[PipeNet] fluid consume via port {} owner {} rejected: port "
                    "is resource kind {}, not FLUID",
                    portId, ownerId, static_cast<int>(key.resource_kind));
                return blocked;
            }
        }
        spdlog::debug("[PipeNet] fluid consume via unknown port {} owner {}",
                      portId, ownerId);
        return blocked;
    }
    if (port->role != gtnh::common::PortRole::SINK) {
        spdlog::warn(
            "[PipeNet] fluid consume via port {} owner {} rejected: role is not SINK",
            portId, ownerId);
        return blocked;
    }
    return consumeFluid(findNodeAtPosition(port->x, port->y, port->z),
                        requestId, fluidId, amount);
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

        if (node.domains[kHuDomainIdx].is_source &&
            node.heatStored > node.heatCapacity * 0.9) {
            heatSources.push_back(nid);
        }

        if (node.domains[kHuDomainIdx].is_sink &&
            node.heatStored < node.heatCapacity) {
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
            if (const auto& srcDomain = sourceNode.domains[kHuDomainIdx]; srcDomain.rate > 0) {
                take = std::min(take, srcDomain.rate);
            }
            if (const auto& sinkDomain = node.domains[kHuDomainIdx]; sinkDomain.rate > 0) {
                take = std::min(take, sinkDomain.rate);
            }
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
        if (nodeIt->second.domains[kHuDomainIdx].is_sink) anySink = true;
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
