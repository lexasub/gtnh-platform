#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <asio.hpp>

#include "CableGraph.h"
#include "CableTypes.h"
#include "Client/MessageRouterClient.h"
#include "PipeBlockIds.h"
#include "PipeNetwork.h"

namespace gtnh {
namespace pipe_network {

struct NodeState {
  uint64_t protocol_id = 0;
  int32_t energy = 0;
  int32_t capacity = 0;
  int32_t max_input = 0;
  int32_t max_output = 0;
  int32_t tier = 0;
  int32_t type = 0;
  bool is_source = false;
  bool is_sink = false;
};

class PipeNetworkService {
public:
  PipeNetworkService(MessageRouterClient &router, asio::io_context &io);
  ~PipeNetworkService();

  void Start();
  void Stop();

private:
  MessageRouterClient &router_;
  asio::io_context &io_;
  asio::steady_timer tick_timer_;
  std::atomic<bool> running_{false};

  static constexpr int TICK_INTERVAL_MS = 100;

  pipenet::PipeNetworkManager network_manager_;
  std::unordered_map<uint64_t, NodeState> node_states_;
  std::unordered_map<uint64_t, uint64_t> protocol_to_mgr_;
  // pos_key → PipeNetworkManager node_id
  std::unordered_map<uint64_t, uint64_t> pipe_nodes_;
  CableGraph cable_graph_;

  void scheduleTick();
  void tick();

  void onRouterMessage(const std::string &topic,
                       const std::vector<uint8_t> &data);

  // Energy node handlers
  void handleNodeUpdate(const std::vector<uint8_t> &data);
  void handleCheckRequest(const std::vector<uint8_t> &data);
  void handleConsumeRequest(const std::vector<uint8_t> &data);

  // Fluid node handlers
  void handleFluidNodeUpdate(const std::vector<uint8_t> &data);
  void handleFluidCheckRequest(const std::vector<uint8_t> &data);
  void handleFluidConsumeRequest(const std::vector<uint8_t> &data);

  // Item node handlers
  void handleItemNodeUpdate(const std::vector<uint8_t> &data);
  void handleItemTransferRequest(const std::vector<uint8_t> &data);

  // Block change handler (pipe auto-detection)
  void handleBlockChanged(const std::vector<uint8_t> &data);
  static bool isPipeBlock(uint16_t block_id);
  static bool isCableBlock(uint16_t block_id);
  static uint64_t posKey(int32_t x, int32_t y, int32_t z);
};

} // namespace pipe_network
} // namespace gtnh
