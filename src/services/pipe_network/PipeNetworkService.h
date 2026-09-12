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
#include "PipeConsumeTransactions.h"
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
  uint32_t fluid_id = 0;
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
  static constexpr const char* PERSIST_DIR = ".pipe_network_persist/";
  static constexpr int PERSIST_INTERVAL_TICKS = 50;
  int tick_counter_ = 0;

  pipenet::PipeNetworkManager network_manager_;
  std::unordered_map<uint64_t, NodeState> node_states_;
  std::unordered_map<uint64_t, uint64_t> protocol_to_mgr_;
  // Pending shortfall consumes keyed by drain request id (3.4.2/3.4.3);
  // bounded by kPendingConsumeTtlTicks expiry, retried with doubling backoff,
  // and cancelled on port removal/re-registration (3.5.3/3.5.4).
  PipeConsumeTracker consume_tracker_;
  // Monotonic service tick driving pending-consume TTL expiry.
  uint64_t service_tick_ = 0;
  // Fresh transaction ids for the consume and drain legs. Seeded from wall
  // clock so ids never repeat across service restarts (owner-side replay
  // caches may outlive this process).
  uint64_t next_txn_id_ = 0;
  // Warn-once markers for manager-rejected port register/remove messages
  // (2.5.3): producers may republish every tick, so rejections log once per
  // port, not per tick. Epoch is normalized to 0 in the marker key; a later
  // successful registration for the same port clears the marker.
  std::unordered_set<gtnh::common::ResourcePortRegistrationKey>
      stale_epoch_warned_;
  // pos_key → PipeNetworkManager node_id (pipe blocks, from world.blocks.changed
  // or authoritative chunk snapshots).
  std::unordered_map<uint64_t, uint64_t> pipe_nodes_;
  // pos_key → connection mask (meta) for pipe/cable blocks
  std::unordered_map<uint64_t, uint8_t> pipe_meta_;
  struct ChunkKey {
    int32_t x;
    int32_t y;
    int32_t z;
    bool operator==(const ChunkKey&) const = default;
  };
  struct ChunkKeyHash {
    size_t operator()(const ChunkKey& key) const noexcept {
      size_t h = static_cast<size_t>(static_cast<uint32_t>(key.x));
      h = h * 31u + static_cast<size_t>(static_cast<uint32_t>(key.y));
      h = h * 31u + static_cast<size_t>(static_cast<uint32_t>(key.z));
      return h;
    }
  };
  // Chunk coordinate → pipe positions seen in the last authoritative snapshot.
  std::unordered_map<ChunkKey, std::unordered_set<uint64_t>, ChunkKeyHash>
      chunk_pipe_positions_;
  // pos_key → node_id (registered machine nodes, from *.node.update)
  std::unordered_map<uint64_t, uint64_t> machine_nodes_;
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

  // Typed resource-port contract (refactor-fluid-port-accounting)
  void handleResourcePortRegister(const std::vector<uint8_t> &data);
  void handleResourcePortRemove(const std::vector<uint8_t> &data);
  void handleResourceDrainResponse(const std::vector<uint8_t> &data);

  // Publishes a legacy FluidConsumeResp (consumed/remaining) to the consumer.
  void publishFluidConsumeResponse(int32_t consumed, int32_t remaining);
  // Telemetry-only fluid.flow event for an applied source drain (3.3.3).
  void publishFluidFlowTelemetry(const PendingConsume &pending,
                                 int32_t amount);

  // Item node handlers
  void handleItemNodeUpdate(const std::vector<uint8_t> &data);
  void handleItemTransferRequest(const std::vector<uint8_t> &data);

  // Block change handler (pipe auto-detection).
  void handleBlockChanged(const std::vector<uint8_t> &data);
  // Register all pipe blocks from a chunk snapshot. The snapshot uses the
  // existing CompressedChunkData wire format; no new protocol is required.
  void handleChunkLoaded(const std::vector<uint8_t> &data);
  void registerPipeBlock(int32_t x, int32_t y, int32_t z,
                         uint16_t block_id, uint8_t meta);
  void refreshPipeConnections(uint64_t nodeId, int32_t x, int32_t y, int32_t z,
                              uint16_t block_id, uint8_t meta);

  // Mask-aware item/fluid/heat edge creation: connects the node at (x,y,z) to
  // compatible pipe/machine neighbors, gated by per-face connection masks.
  void connectNodeNeighbors(uint64_t sourceNodeId, int32_t x, int32_t y, int32_t z,
                           uint8_t sourceMeta, bool isItem, bool isHeat,
                           bool sourceIsPipe);

  static bool isPipeBlock(uint16_t block_id);
  static bool isCableBlock(uint16_t block_id);
  static uint64_t posKey(int32_t x, int32_t y, int32_t z);

  // Machine config handler (side_config sync from wrench)
  void handleMachineConfigUpdated(const std::vector<uint8_t> &data);

  // Pipe wrench handler (evaluate + report connection state)
  void handlePipeWrenchAction(const std::vector<uint8_t> &data);

  // Debug pipe-contents query: report the fluid state of the node at pos
  // exactly as stored (read-only, no graph mutation).
  void handlePipeContentsRequest(const std::vector<uint8_t> &data);

  void loadPersistentState();
};

} // namespace pipe_network
} // namespace gtnh
