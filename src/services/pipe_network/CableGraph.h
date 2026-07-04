#pragma once
#include "CableTypes.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gtnh {
namespace pipe_network {

struct EnergyPacket {
  uint32_t voltage;
  uint8_t ampCount;
  uint64_t sourceId;
  uint64_t targetId;
  uint8_t tickIssued;
};

struct ExplodedNode {
  uint64_t nodeId;
  int32_t x, y, z;
  float temperature;
};

class CableGraph {
public:
  struct CableNode {
    uint64_t id;
    int32_t x, y, z;
    uint8_t tier;
    uint32_t maxVoltage;
    uint32_t ampacity;
    uint32_t packetsThisTick = 0;
    float temperature = 0.0f;
    float lossPerBlock = 0.0f;
    uint16_t blockId = 0;
    uint32_t maxSeenVoltage = 0;
    std::vector<EnergyPacket> packetsIn;
    std::vector<EnergyPacket> packetsOut;
  };

  void addCableNode(uint64_t nodeId, const CableDef &def, int32_t x, int32_t y,
                    int32_t z);
  void removeCableNode(uint64_t nodeId);
  void rebuildGraph();
  void injectPacket(const EnergyPacket &packet, uint64_t startNodeId);
  std::vector<EnergyPacket> collectPackets(uint64_t machineNodeId);
  void tick();
  void setCableParams(uint64_t nodeId, const CableDef &def);
  void registerGenerator(uint64_t genEntityId, int32_t x, int32_t y, int32_t z);
  void registerMachine(uint64_t machineEntityId, int32_t x, int32_t y,
                       int32_t z);
  void unregisterGenerator(uint64_t genEntityId);
  void unregisterMachine(uint64_t machineEntityId);
  uint64_t findAdjacentCable(int32_t x, int32_t y, int32_t z);
  bool isRegisteredGenerator(uint64_t genEntityId) const;

  // Get nodes that exploded during the last tick()
  const std::vector<ExplodedNode> &getExplodedNodes() const {
    return m_explodedThisTick;
  }

private:
  std::unordered_map<uint64_t, CableNode> m_nodes;
  std::vector<std::vector<uint64_t>> m_cableNetworks;
  std::unordered_map<uint64_t, uint64_t> m_generatorToCable;
  std::unordered_map<uint64_t, uint64_t> m_machineToCable;
  std::vector<ExplodedNode> m_explodedThisTick;
  void processOverheat();
  std::vector<uint64_t> findPath(uint64_t fromCableNode,
                                 uint64_t toMachineNode);
};

} // namespace pipe_network
} // namespace gtnh