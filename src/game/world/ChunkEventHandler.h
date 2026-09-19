#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace simcore {

class SimulationEngine;

class ChunkEventHandler {
public:
  explicit ChunkEventHandler(std::shared_ptr<SimulationEngine> engine);

  void handle(const std::vector<uint8_t> &flatbuffer_data);

private:
  std::shared_ptr<SimulationEngine> engine_;
};

} // namespace simcore