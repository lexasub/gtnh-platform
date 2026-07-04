// gateway.h
#pragma once

#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/router_client.h>
#include <gtnh/net/server.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Gateway ↔ Client wire protocol
//
// Frame: [4 bytes: payload size BE] [1 byte: message type] [FlatBuffer data]
//
// Types (client → gateway):
//   1 = PlayerAction (FlatBuffer: Protocol::PlayerAction)
//   6 = InventoryAction (FlatBuffer: Protocol::InventoryAction)
//  11 = SetBlockAction (FlatBuffer: Protocol::SetBlockAction)
//  13 = ToolAction (FlatBuffer: Protocol::ToolAction)
//
// Types (gateway → client):
//   2 = ChunkData (FlatBuffer: Protocol::ChunkData)
//   3 = EntitySnapshot (FlatBuffer: Protocol::EntitySnapshot)
//   6 = InventoryUpdate (FlatBuffer: Protocol::InventoryUpdate)
//  14 = ToolActionResp (FlatBuffer: Protocol::ToolActionResp)
// ---------------------------------------------------------------------------
namespace GatewayMsg { // TODO move to protocol (flatbuffers)
inline constexpr uint8_t kPlayerAction = 1;
inline constexpr uint8_t kChunkSnapshot = 2;
inline constexpr uint8_t kEntitySnapshot = 3;
inline constexpr uint8_t kBlockUpdate = 4;
inline constexpr uint8_t kBlockAck = 5;
inline constexpr uint8_t kInventoryUpdate = 6;
inline constexpr uint8_t kInventoryAction = 7;
inline constexpr uint8_t kBlockEntityUpdate = 8;
inline constexpr uint8_t kCraftRequest = 9;
inline constexpr uint8_t kCraftResponse = 10;
inline constexpr uint8_t kSetBlockAction = 11;
inline constexpr uint8_t kCompressedChunkData = 12;
inline constexpr uint8_t kToolAction = 13;
inline constexpr uint8_t kToolActionResp = 14;
inline constexpr uint8_t kSetMachineSlot = 15;
inline constexpr uint8_t kSetMachineSlotResp = 16;
inline constexpr uint8_t kRecipeCompleted = 17;
inline constexpr uint8_t kChestOpenReq = 18;
inline constexpr uint8_t kChestOpenResp = 19;
inline constexpr uint8_t kQuestProgressUpdate = 20;
inline constexpr uint8_t kQuestUnlockNotification = 21;
inline constexpr uint8_t kQuestCompletedNotification = 22;
} // namespace GatewayMsg

// ---------------------------------------------------------------------------
// Interest management
// ---------------------------------------------------------------------------
struct ChunkCoord {
  int32_t x, y, z;
};

struct PlayerInterest {
  int32_t center_x = 0, center_y = 0, center_z = 0;
  int radius = 8; // chunks

  bool ShouldSendChunk(ChunkCoord cc) const {
    return std::abs(cc.x - center_x) <= radius &&
           std::abs(cc.y - center_y) <= radius &&
           std::abs(cc.z - center_z) <= radius;
  }
};

// Top-level Gateway: owns io_uring rings, listens for GameClient connections,
// connects to MessageRouter, relays messages bidirectionally.
//
class IoUringGateway {
public:
  IoUringGateway() = default;
  ~IoUringGateway();

  IoUringGateway(const IoUringGateway &) = delete;
  IoUringGateway &operator=(const IoUringGateway &) = delete;

  bool init();
  bool listen(uint16_t ctrl_port, uint16_t bulk_port);
  bool connect_router(const std::string &host, uint16_t port);

  void subscribe(const std::string &topic);
  void publish(const std::string &topic, const uint8_t *data, size_t len);

  void send_to_client_ctrl(uint8_t msg_type, const uint8_t *data, size_t len);
  void send_to_client_bulk(uint8_t msg_type, const uint8_t *data, size_t len);
  void send_to_client_ctrl_raw(std::shared_ptr<std::vector<uint8_t>> frame);
  void send_to_client_bulk_raw(std::shared_ptr<std::vector<uint8_t>> frame);
  void send_to_client_ctrl_raw(uint8_t msg_type, const uint8_t *data,
                               size_t len);
  bool send_to_client_bulk_raw(uint8_t msg_type, const uint8_t *data,
                               size_t len);

  PlayerInterest *client_interest();
  bool has_client() const;

  void publish_player_joined();
  void publish_player_left();

  void shutdown();
  void sendHeartbeat();

  std::function<void(const uint8_t *data, size_t len)> on_client_message;
  std::function<void(const std::string &topic, const uint8_t *data, size_t len)>
      on_router_message;

private:
  void on_router_publish(const std::string &topic,
                         std::shared_ptr<std::vector<uint8_t>> data);
  void on_client_ctrl_message(uint8_t msg_type, const uint8_t *data,
                              size_t len);
  void on_client_bulk_message(uint8_t msg_type, const uint8_t *data,
                              size_t len);

  gtnh::net::TcpServer ctrl_server_;
  gtnh::net::TcpServer bulk_server_;
  gtnh::net::RouterClient router_;

  std::unique_ptr<gtnh::net::IoUringConnection> client_ctrl_;
  std::unique_ptr<gtnh::net::IoUringConnection> client_bulk_;
  gtnh::net::TagAllocator tag_alloc_;

  mutable std::mutex client_ctrl_mutex_;
  mutable std::mutex client_bulk_mutex_;

  uint16_t ctrl_port_ = 0;
  uint16_t bulk_port_ = 0;

  // Client state for inventory chain
  mutable std::mutex client_state_mutex_;

  // Session generation bumped on each accept().  Captured in on_closed
  // so stale callbacks from old sessions don't corrupt the new session's
  // player_id_known_ flag (race: old on_closed fires after new accept).
  std::atomic<uint64_t> session_gen_{0};

  uint64_t client_player_id_ = 0;
  int32_t last_x_ = 0, last_y_ = 0, last_z_ = 0;
  bool player_id_known_ = false;
};
