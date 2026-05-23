#include "CableGraph.h"
#include "CableLoss.h"
#include "CableOverheat.h"
#include <queue>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <array>
#include <spdlog/spdlog.h>

namespace gtnh {
namespace pipe_network {

void CableGraph::addCableNode(uint64_t nodeId, const CableDef& def, int32_t x, int32_t y, int32_t z) {
    CableGraph::CableNode node;
    node.id = nodeId;
    node.x = x;
    node.y = y;
    node.z = z;
    node.tier = def.tier;
    node.maxVoltage = def.max_voltage;
    node.ampacity = def.ampacity;
    node.lossPerBlock = def.loss_per_block;
    node.blockId = def.block_id;
    node.packetsThisTick = 0;
    node.temperature = 0.0f;
    node.maxSeenVoltage = 0;
    m_nodes[nodeId] = node;
}

void CableGraph::removeCableNode(uint64_t nodeId) {
    auto it = m_nodes.find(nodeId);
    if (it == m_nodes.end()) return;
    
    m_generatorToCable.erase(it->second.id);
    m_machineToCable.erase(it->second.id);
    m_nodes.erase(it);
    rebuildGraph();
}

void CableGraph::rebuildGraph() {
    m_cableNetworks.clear();
    std::unordered_set<uint64_t> visited;
    
    for (const auto& pair : m_nodes) {
        uint64_t nodeId = pair.first;
        if (visited.find(nodeId) != visited.end()) continue;
        
        std::vector<uint64_t> component;
        std::queue<uint64_t> q;
        q.push(nodeId);
        visited.insert(nodeId);
        
        while (!q.empty()) {
            uint64_t current = q.front();
            q.pop();
            component.push_back(current);
            
            auto nodeIt = m_nodes.find(current);
            if (nodeIt == m_nodes.end()) continue;
            
            int32_t x = nodeIt->second.x;
            int32_t y = nodeIt->second.y;
            int32_t z = nodeIt->second.z;
            
            // Check 6 adjacent positions
            constexpr int32_t offsets[6][3] = {
                {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
            };
            
            for (const auto& off : offsets) {
                int32_t nx = x + off[0];
                int32_t ny = y + off[1];
                int32_t nz = z + off[2];
                
                for (const auto& pair2 : m_nodes) {
                    const auto& node2 = pair2.second;
                    if (node2.x == nx && node2.y == ny && node2.z == nz) {
                        if (visited.find(node2.id) == visited.end()) {
                            visited.insert(node2.id);
                            q.push(node2.id);
                        }
                        break;
                    }
                }
            }
        }
        
        if (!component.empty()) {
            m_cableNetworks.push_back(component);
        }
    }
}

void CableGraph::injectPacket(const EnergyPacket& packet, uint64_t startNodeId) {
    auto it = m_nodes.find(startNodeId);
    if (it != m_nodes.end()) {
        it->second.packetsIn.push_back(packet);
    }
}

std::vector<EnergyPacket> CableGraph::collectPackets(uint64_t machineNodeId) {
    std::vector<EnergyPacket> collected;
    
    auto machineIt = m_machineToCable.find(machineNodeId);
    if (machineIt == m_machineToCable.end()) return collected;
    
    uint64_t cableNodeId = machineIt->second;
    auto cableIt = m_nodes.find(cableNodeId);
    if (cableIt == m_nodes.end()) return collected;
    
    // Drain packetsOut from the machine's adjacent cable node
    collected = std::move(cableIt->second.packetsOut);
    cableIt->second.packetsOut.clear();
    
    return collected;
}

void CableGraph::tick() {
    m_explodedThisTick.clear();

    for (auto& pair : m_nodes) {
        pair.second.packetsThisTick = 0;
        pair.second.maxSeenVoltage = 0;
    }

    // Process each cable network
    for (const auto& network : m_cableNetworks) {
        for (uint64_t nodeId : network) {
            auto nodeIt = m_nodes.find(nodeId);
            if (nodeIt == m_nodes.end()) {
                continue;
            }

            std::vector<EnergyPacket> nodePackets = std::move(nodeIt->second.packetsIn);
            nodeIt->second.packetsIn.clear();

            for (auto packet : nodePackets) {
                uint64_t targetCableNode = 0;
                if (packet.targetId != 0) {
                    auto machineIt = m_machineToCable.find(packet.targetId);
                    if (machineIt != m_machineToCable.end()) {
                        targetCableNode = machineIt->second;
                    } else {
                        continue;
                    }
                }

                std::vector<uint64_t> path;
                uint64_t sourceCableNode = nodeId;

                if (packet.targetId == 0) {
                    std::unordered_set<uint64_t> visited;
                    std::queue<std::pair<uint64_t, std::vector<uint64_t>>> q;
                    q.push({sourceCableNode, {sourceCableNode}});
                    visited.insert(sourceCableNode);

                    while (!q.empty()) {
                        auto current = q.front();
                        q.pop();
                        uint64_t curId = current.first;
                        std::vector<uint64_t> curPath = current.second;

                        auto curIt = m_nodes.find(curId);
                        if (curIt == m_nodes.end()) continue;

                        bool isMachineAdjacent = false;
                        for (const auto& pair : m_machineToCable) {
                            if (pair.second == curId) {
                                isMachineAdjacent = true;
                                break;
                            }
                        }

                        if (isMachineAdjacent) {
                            path = curPath;
                            break;
                        }

                        int32_t cx = curIt->second.x;
                        int32_t cy = curIt->second.y;
                        int32_t cz = curIt->second.z;
                        constexpr int32_t offsets[6][3] = {
                            {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
                        };

                        for (const auto& off : offsets) {
                            int32_t nx = cx + off[0];
                            int32_t ny = cy + off[1];
                            int32_t nz = cz + off[2];

                            for (const auto& pair : m_nodes) {
                                const auto& other = pair.second;
                                if (other.x == nx && other.y == ny && other.z == nz &&
                                    visited.find(other.id) == visited.end()) {
                                    visited.insert(other.id);
                                    std::vector<uint64_t> newPath = curPath;
                                    newPath.push_back(other.id);
                                    q.push({other.id, newPath});
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    path = findPath(sourceCableNode, targetCableNode);
                }

                if (path.empty()) {
                    continue;
                }

                EnergyPacket workingPacket = packet;
                uint64_t currentNode = path[0];
                for (size_t i = 1; i < path.size(); ++i) {
                    uint64_t nextNode = path[i];
                    auto currentIt = m_nodes.find(currentNode);
                    auto nextIt = m_nodes.find(nextNode);
                    if (currentIt == m_nodes.end() || nextIt == m_nodes.end()) continue;

                    if (workingPacket.voltage > nextIt->second.maxSeenVoltage) {
                        nextIt->second.maxSeenVoltage = workingPacket.voltage;
                    }

                    if (workingPacket.voltage > nextIt->second.maxVoltage) {
                        continue;
                    }

                    float effVolt = effectiveVoltage(
                        1.0f,
                        currentIt->second.lossPerBlock,
                        static_cast<float>(workingPacket.voltage));
                    if (effVolt <= 0.0f) {
                        continue;
                    }
                    workingPacket.voltage = static_cast<uint32_t>(std::floor(effVolt));

                    currentIt->second.packetsThisTick++;
                    if (currentIt->second.packetsThisTick > currentIt->second.ampacity) {
                        continue;
                    }

                    nextIt->second.packetsOut.push_back(workingPacket);
                    currentNode = nextNode;
                }
            }
        }
    }

    processOverheat();
}

void CableGraph::processOverheat() {
    for (auto& pair : m_nodes) {
        auto& node = pair.second;
        OverheatResult result = calculateOverheat(
            node.maxSeenVoltage,
            node.maxVoltage,
            static_cast<int32_t>(node.packetsThisTick),
            node.ampacity,
            node.temperature
        );
        node.temperature = result.temperature;
        if (result.exploded) {
            spdlog::warn("[CableGraph] cable node {} at ({},{},{}) exploded! "
                         "temp={}, maxSeenVoltage={}, packetsThisTick={}",
                         node.id, node.x, node.y, node.z,
                         node.temperature, node.maxSeenVoltage, node.packetsThisTick);
            m_explodedThisTick.push_back({node.id, node.x, node.y, node.z, node.temperature});
        }
    }

    for (const auto& exploded : m_explodedThisTick) {
        removeCableNode(exploded.nodeId);
    }
}

void CableGraph::setCableParams(uint64_t nodeId, const CableDef& def) {
    auto it = m_nodes.find(nodeId);
    if (it == m_nodes.end()) return;
    
    it->second.maxVoltage = def.max_voltage;
    it->second.ampacity = def.ampacity;
    it->second.tier = def.tier;
}

void CableGraph::registerGenerator(uint64_t genEntityId, int32_t x, int32_t y, int32_t z) {
    uint64_t cableNode = findAdjacentCable(x, y, z);
    if (cableNode) {
        m_generatorToCable[genEntityId] = cableNode;
    }
}

void CableGraph::registerMachine(uint64_t machineEntityId, int32_t x, int32_t y, int32_t z) {
    uint64_t cableNode = findAdjacentCable(x, y, z);
    if (cableNode) {
        m_machineToCable[machineEntityId] = cableNode;
    }
}

void CableGraph::unregisterGenerator(uint64_t genEntityId) {
    m_generatorToCable.erase(genEntityId);
}

void CableGraph::unregisterMachine(uint64_t machineEntityId) {
    m_machineToCable.erase(machineEntityId);
}

uint64_t CableGraph::findAdjacentCable(int32_t x, int32_t y, int32_t z) {
    constexpr int32_t offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };
    
    for (const auto& off : offsets) {
        int32_t nx = x + off[0];
        int32_t ny = y + off[1];
        int32_t nz = z + off[2];
        
        for (const auto& pair : m_nodes) {
            const auto& node = pair.second;
            if (node.x == nx && node.y == ny && node.z == nz) {
                return node.id;
            }
        }
    }
    
    return 0;
}

bool CableGraph::isRegisteredGenerator(uint64_t genEntityId) const {
    return m_generatorToCable.find(genEntityId) != m_generatorToCable.end();
}

std::vector<uint64_t> CableGraph::findPath(uint64_t fromCableNode, uint64_t toMachineNode) {
    std::vector<uint64_t> path;
    if (fromCableNode == 0 || toMachineNode == 0) return path;
    
    std::unordered_set<uint64_t> visited;
    std::queue<std::pair<uint64_t, std::vector<uint64_t>>> q;
    
    q.push({fromCableNode, {fromCableNode}});
    visited.insert(fromCableNode);
    
    while (!q.empty()) {
        auto current = q.front();
        q.pop();
        uint64_t nodeId = current.first;
        std::vector<uint64_t> currentPath = current.second;
        
        if (nodeId == toMachineNode) {
            path = currentPath;
            break;
        }
        
        auto nodeIt = m_nodes.find(nodeId);
        if (nodeIt == m_nodes.end()) continue;
        
        int32_t x = nodeIt->second.x;
        int32_t y = nodeIt->second.y;
        int32_t z = nodeIt->second.z;
        
        constexpr int32_t offsets[6][3] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
        };
        
        for (const auto& off : offsets) {
            int32_t nx = x + off[0];
            int32_t ny = y + off[1];
            int32_t nz = z + off[2];
            
            for (const auto& pair : m_nodes) {
                const auto& other = pair.second;
                if (other.x == nx && other.y == ny && other.z == nz && 
                    visited.find(other.id) == visited.end()) {
                    visited.insert(other.id);
                    std::vector<uint64_t> newPath = currentPath;
                    newPath.push_back(other.id);
                    q.push({other.id, newPath});
                    break;
                }
            }
        }
    }
    
    return path;
}

} // namespace pipe_network
} // namespace gtnh