#pragma once

#include "services/storage_interfaces/IEntityStateStorage.h"
#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace simcore {

class EntityStateStoreClient {
public:
  explicit EntityStateStoreClient(asio::io_context &io);
  ~EntityStateStoreClient();

  void Connect(const std::string &host, uint16_t port);
  void Disconnect();

  // RPC: LoadEntityState
  struct EntityStateData {
    std::vector<uint8_t> state;
  };
  using LoadEntityStateCallback =
      std::function<void(const EntityStateData &state)>;
  void LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                       uint16_t entity_type, LoadEntityStateCallback callback);

  // RPC: SaveEntityState
  using SaveEntityStateCallback = std::function<void(bool success)>;
  void SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                       uint16_t entity_type,
                       const std::vector<uint8_t> &stateData,
                       SaveEntityStateCallback callback);

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

  std::unordered_map<uint32_t,
                     std::function<void(const std::vector<uint8_t> &)>>
      pending_callbacks_;
};

} // namespace simcore
