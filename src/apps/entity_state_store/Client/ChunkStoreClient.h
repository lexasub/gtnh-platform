#pragma once

#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gtnh {
namespace entity_state_store {

class ChunkStoreClient {
public:
  explicit ChunkStoreClient(asio::io_context &io);
  ~ChunkStoreClient();

  void Connect(const std::string &host, uint16_t port);
  void Disconnect();

  // RPC: SetBlock (simple, no CAS)
  using SetBlockCallback = std::function<void(bool success)>;
  void SetBlock(int32_t x, int32_t y, int32_t z, uint16_t block_id,
                uint8_t meta, SetBlockCallback callback);

  // RPC: GetBlock
  struct BlockData {
    uint16_t block_id;
    uint8_t meta;
    uint32_t mb_id;
  };
  using GetBlockCallback = std::function<void(const BlockData &block)>;
  void GetBlock(int32_t x, int32_t y, int32_t z, GetBlockCallback callback);

  bool IsConnected() const;

private:
  void doConnect(const std::string &host, uint16_t port);
  void onConnect(const asio::error_code &ec);
  void writeFrame(const std::vector<uint8_t> &frame);
  void readFrame();

  asio::io_context &io_;
  asio::ip::tcp::socket socket_;
  std::string host_;
  uint16_t port_;
  bool connected_ = false;
  bool stopped_ = false;
  std::atomic<uint32_t> next_req_id_{1};

  // Pending callbacks
  std::unordered_map<uint32_t,
                     std::function<void(const std::vector<uint8_t> &)>>
      pending_callbacks_;
};

} // namespace entity_state_store
} // namespace gtnh
