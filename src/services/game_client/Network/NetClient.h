#pragma once

#include "../Common/Inventory.h"
#include "../Common/Types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace gtnh::net {
class IoUringConnection;
}

namespace Protocol {
enum PlayerActionType : uint8_t;
enum ToolActionType : uint8_t;
} // namespace Protocol

struct ChunkCoord;
class ChunkView;

namespace GatewayMsg {
inline constexpr uint8_t kPlayerAction = 1;
inline constexpr uint8_t kChunkSnapshot = 2;
inline constexpr uint8_t kEntitySnap = 3;
inline constexpr uint8_t kBlockUpdate = 4;
inline constexpr uint8_t kBlockEntityUpdate = 8;
inline constexpr uint8_t kBlockAck = 5;
inline constexpr uint8_t kInventoryUpdate = 6;
inline constexpr uint8_t kInventoryAction = 7;
inline constexpr uint8_t kCraftRequest = 9;
inline constexpr uint8_t kCraftResponse = 10;
inline constexpr uint8_t kSetBlockAction = 11;
inline constexpr uint8_t kCompressedChunkData = 12;
inline constexpr uint8_t kToolAction = 13;
inline constexpr uint8_t kToolActionResp = 14;
inline constexpr uint8_t kRecipeCompleted = 17;
inline constexpr uint8_t kMachineOpenReq = 18; // was kChestSaveReq (dead, removed)
inline constexpr uint8_t kChestOpenReq = 19;
inline constexpr uint8_t kChestCloseReq = 45;
inline constexpr uint8_t kMachineCloseReq = 46;
inline constexpr uint8_t kQuestProgressUpdate = 20;
inline constexpr uint8_t kQuestUnlockNotification = 21;
inline constexpr uint8_t kQuestCompletedNotification = 22;
inline constexpr uint8_t kMultiblockEvent = 23;
inline constexpr uint8_t kQuestCompleteRequest = 24;
inline constexpr uint8_t kQuestEraTransition = 25;
inline constexpr uint8_t kQuestExchangeRequest = 26;
inline constexpr uint8_t kQuestExchangeResponse = 27;
inline constexpr uint8_t kQuestExchangeCooldownGet = 28;
inline constexpr uint8_t kQuestExchangeCooldown = 29;
inline constexpr uint8_t kGameModeChange = 30;
inline constexpr uint8_t kStartScenarioReq = 31;
inline constexpr uint8_t kStartScenarioResp = 32;
inline constexpr uint8_t kQuestBookOpen = 33;
inline constexpr uint8_t kRecipeCheckReq = 34;
inline constexpr uint8_t kRecipeCheckResp = 35;
inline constexpr uint8_t kRecipeCatalogReq = 36;
inline constexpr uint8_t kRecipeCatalogResp = 37;
inline constexpr uint8_t kRecipeItemReq = 38;
inline constexpr uint8_t kRecipeItemResp = 39;
inline constexpr uint8_t kRecipeMachineReq = 40;
inline constexpr uint8_t kRecipeMachineResp = 41;
inline constexpr uint8_t kBlockActionDirective = 42;
inline constexpr uint8_t kGridUpdate = 43;
inline constexpr uint8_t kWorkbenchOpenReq = 44;
} // namespace GatewayMsg

class NetClient : public std::enable_shared_from_this<NetClient> {
public:
  using ChunkCallback =
      std::function<void(std::shared_ptr<ChunkView>, ChunkCoord)>;
  using BlockUpdateCallback =
      std::function<void(BlockPos, uint16_t, uint8_t, uint32_t)>;
  using BlockAckCallback = std::function<void(BlockPos pos, uint8_t status,
                                              uint16_t block_id, uint8_t meta,
                                              uint32_t request_id,
                                              uint8_t action_type)>;
  using BlockActionDirectiveCallback = std::function<void(
      BlockPos pos, uint8_t directive, uint16_t block_id, uint32_t request_id,
      uint8_t action_type)>;
  using InventoryUpdateCallback =
      std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
  using CraftResponseCallback =
      std::function<void(bool, uint16_t, uint8_t, uint16_t, const std::string &,
                         const std::array<ItemStack, 9> &)>;
  using BlockEntityUpdateCallback =
      std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
  using RecipeCompletedCallback =
      std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
  // Server-driven recipe query replies (payload = Protocol::RecipeFrame →
  // RecipeReply). Parsing/caching happens in ServerRecipeDB, so these are thin
  // raw-buffer callbacks.
  using RecipeQueryCallback =
      std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
  using ToolActionRespCallback = std::function<void(
      bool, uint8_t, const std::vector<uint8_t> &, const std::string &)>;
    using MultiblockEventCallback =
        std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
    using GridUpdateCallback =
        std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
    void SetGridUpdateCallback(GridUpdateCallback cb) {
        onGridUpdate_ = std::move(cb);
    }
    using QuestUpdateCallback =
      std::function<void(uint8_t, std::shared_ptr<std::vector<uint8_t>>)>;
  using GameModeChangeCallback = std::function<void(uint8_t new_mode)>;
  using StartScenarioRespCallback = std::function<void(
      std::shared_ptr<std::vector<uint8_t>>)>;
  using ReconnectCallback = std::function<void()>;

  explicit NetClient();
  ~NetClient();

  // ---- Connection --------------------------------------------------------

  bool Connect(const std::string &host, uint16_t ctrl_port, uint16_t bulk_port);
  void Disconnect();
  bool IsConnected() const { return connected_ctrl_ || connected_bulk_; }
  bool IsCtrlConnected() const { return connected_ctrl_; }
  bool IsBulkConnected() const {
    return connected_bulk_ && reconnect_attempts_ < max_reconnect_attempts_;
  }

  // ---- Poll (call each frame from game thread) ---------------------------

  void Poll();

  // ---- Callbacks ---------------------------------------------------------

  void SetChunkCallback(ChunkCallback cb) { onChunkReceived_ = std::move(cb); }
  void SetBlockUpdateCallback(BlockUpdateCallback cb) {
    onBlockUpdate_ = std::move(cb);
  }
  void SetBlockAckCallback(BlockAckCallback cb) { onBlockAck_ = std::move(cb); }
  void SetBlockActionDirectiveCallback(BlockActionDirectiveCallback cb) {
    onBlockActionDirective_ = std::move(cb);
  }
  void SetInventoryUpdateCallback(InventoryUpdateCallback cb) {
    onInventoryUpdate_ = std::move(cb);
  }
  void SetBlockEntityUpdateCallback(BlockEntityUpdateCallback cb) {
    onBlockEntityUpdate_ = std::move(cb);
  }
  void SetRecipeCompletedCallback(RecipeCompletedCallback cb) {
    onRecipeCompleted_ = std::move(cb);
  }
  void SetRecipeCheckRespCallback(RecipeQueryCallback cb) {
    onRecipeCheckResp_ = std::move(cb);
  }
  void SetRecipeCatalogRespCallback(RecipeQueryCallback cb) {
    onRecipeCatalogResp_ = std::move(cb);
  }
  void SetRecipesForItemRespCallback(RecipeQueryCallback cb) {
    onRecipesForItemResp_ = std::move(cb);
  }
  void SetRecipesForMachineRespCallback(RecipeQueryCallback cb) {
    onRecipesForMachineResp_ = std::move(cb);
  }
  void SetCraftResponseCallback(CraftResponseCallback cb) {
    onCraftResponse_ = std::move(cb);
  }
  void SetToolActionRespCallback(ToolActionRespCallback cb) {
    onToolActionResp_ = std::move(cb);
  }
  void SetMultiblockEventCallback(MultiblockEventCallback cb) {
    onMultiblockEvent_ = std::move(cb);
  }
  void SetQuestUpdateCallback(QuestUpdateCallback cb) {
    onQuestUpdate_ = std::move(cb);
  }
  void SetGameModeChangeCallback(GameModeChangeCallback cb) { onGameModeChange_ = std::move(cb); }
  void SetStartScenarioRespCallback(StartScenarioRespCallback cb) { onStartScenarioResp_ = std::move(cb); }
  void SetReconnectCallback(ReconnectCallback cb) {
    onReconnect_ = std::move(cb);
  }

  // ---- outbound messages -------------------------------------------------

  void RequestChunk(const ChunkCoord &coord);
  void SendPlayerAction(uint64_t player_id, Protocol::PlayerActionType action,
                        int32_t x, int32_t y, int32_t z, uint16_t item_id = 0,
                        uint8_t count = 0);
  void SendBlockAction(Protocol::PlayerActionType action, int32_t x, int32_t y,
                       int32_t z, uint16_t currentBlockID,
                       uint16_t held_item = 0, uint8_t face = 0,
                       uint64_t player_id = 0);
  void SendInventoryAction(uint64_t player_id, uint8_t action_type,
                           uint8_t button, uint8_t mods,
                           uint8_t container_id, uint16_t slot, uint8_t count);
  void SendCraftRequest(uint64_t player_id, const BlockPos &pos,
                        const std::array<ItemStack, 9> &slots);
  // Server-authoritative container session (Phase B): open/close a chest window.
  void SendChestOpenReq(uint64_t player_id, const BlockPos &pos);
  void SendChestCloseReq(uint64_t player_id, const BlockPos &pos);
  // Server-authoritative container session (Phase C): open/close a machine window.
  void SendMachineOpenReq(uint64_t player_id, int32_t x, int32_t y, int32_t z);
  void SendMachineCloseReq(uint64_t player_id, int32_t x, int32_t y, int32_t z);
  void SendToolAction(uint64_t player_id, Protocol::ToolActionType action,
                      int32_t x, int32_t y, int32_t z, uint8_t face,
                      uint16_t item_id = 0);
  // Manual quest completion (server-authoritative): sends QuestCompleteRequest
  // to the gateway, which forwards to SimulationCore for validation.
  void SendQuestComplete(uint64_t player_id, uint32_t quest_id);
  // Repeatable-market quest exchange: requests a trade (cost deducted,
  // reward granted) for exchange-type quests. MetaDB owns validation.
  void SendQuestExchange(uint64_t player_id, uint32_t quest_id);
  // Queries the current exchange cooldown (seconds remaining) for a quest.
  void SendQuestExchangeCooldownGet(uint64_t player_id, uint32_t quest_id);
  // Notifies the server the player opened the quest book → triggers the
  // server-side inventory check against INVENTORY-type quest objectives.
  void SendQuestBookOpen(uint64_t player_id);
  void SendGameModeChange(uint64_t player_id, uint8_t new_mode);
    void SendStartScenarioReq(uint64_t player_id, uint8_t scenario_index);
    void SendWorkbenchOpenReq(uint64_t player_id, const BlockPos &pos);

  // ── Server-driven recipe queries (payload = Protocol::RecipeFrame) ──
  void SendRecipeCheckReq(uint16_t machine_id,
                          const std::array<ItemStack, 9> &grid,
                          uint32_t req_id);
  void SendRecipeCatalogReq(uint32_t req_id);
  void SendRecipesForItemReq(uint16_t item_id, uint8_t mode, uint32_t req_id);
  void SendRecipesForMachineReq(uint16_t machine_id, uint32_t req_id);

private:
  // ---- Thread-safe message queue -----------------------------------------

  struct QueuedMessage {
    uint8_t type;
    std::shared_ptr<std::vector<uint8_t>> data;
  };

  // ---- Connection helpers ------------------------------------------------

  int tcp_connect(const char *host, uint16_t port);
  bool start_ctrl_connection(int fd);
  bool start_bulk_connection(int fd);

  // ---- Inbound message dispatch (called from Poll on game thread) --------

  void OnMessage(uint8_t msg_type, std::shared_ptr<std::vector<uint8_t>> data);
  void OnBulkMessage(uint8_t msg_type,
                     std::shared_ptr<std::vector<uint8_t>> data);

  // ---- Reconnection state ------------------------------------------------

  std::string host_;
  uint16_t ctrl_port_;
  uint16_t bulk_port_;
  int reconnect_attempts_ = 0;
  const int max_reconnect_attempts_ = 3;
  std::atomic<bool> reconnecting_{false};
  std::atomic<bool> reconnect_requested_{false};

  std::atomic<uint32_t> next_request_id_{1}; // monotonic counter for BlockAck matching

  void request_reconnect();
  void do_reconnect();

  bool ProcessKCompressedChunkData(std::shared_ptr<std::vector<uint8_t>> data);
  void ProcessBlockUpdate(std::shared_ptr<std::vector<uint8_t>> data);
  void ProcessBlockAck(std::shared_ptr<std::vector<uint8_t>> data);
  void ProcessBlockActionDirective(std::shared_ptr<std::vector<uint8_t>> data);
  void OnChunkData(std::shared_ptr<ChunkView> chunk, const ChunkCoord &coord);

  // ---- Internal ----------------------------------------------------------

  void EnqueueWrite(uint8_t msg_type, const void *data, size_t size);
  void drain_queue(std::deque<QueuedMessage> &queue, bool is_bulk);

  std::unique_ptr<gtnh::net::IoUringConnection> ctrl_conn_;
  std::unique_ptr<gtnh::net::IoUringConnection> bulk_conn_;

  std::mutex ctrl_mutex_;
  std::deque<QueuedMessage> ctrl_queue_;
  std::mutex bulk_mutex_;
  std::deque<QueuedMessage> bulk_queue_;

  std::atomic<bool> connected_ctrl_{false};
  std::atomic<bool> connected_bulk_{false};

  // ---- Callbacks ---------------------------------------------------------

  ChunkCallback onChunkReceived_;
  BlockUpdateCallback onBlockUpdate_;
  BlockAckCallback onBlockAck_;
  BlockActionDirectiveCallback onBlockActionDirective_;
  InventoryUpdateCallback onInventoryUpdate_;
  BlockEntityUpdateCallback onBlockEntityUpdate_;
  RecipeCompletedCallback onRecipeCompleted_;
  RecipeQueryCallback onRecipeCheckResp_;
  RecipeQueryCallback onRecipeCatalogResp_;
  RecipeQueryCallback onRecipesForItemResp_;
  RecipeQueryCallback onRecipesForMachineResp_;
  CraftResponseCallback onCraftResponse_;
  ToolActionRespCallback onToolActionResp_;
    MultiblockEventCallback onMultiblockEvent_;
    QuestUpdateCallback onQuestUpdate_;
    GameModeChangeCallback onGameModeChange_;
    StartScenarioRespCallback onStartScenarioResp_;
    GridUpdateCallback onGridUpdate_;
    ReconnectCallback onReconnect_;
};
