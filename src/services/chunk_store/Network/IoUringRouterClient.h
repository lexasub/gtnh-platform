#pragma once

#include <gtnh/net/router_client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ChunkStore;

// io_uring-based Router client that replaces the Asio RouterClient.
// Connects to Go MessageRouter, subscribes to player.actions,
// and publishes compressed chunk data when generation completes.
class IoUringRouterClient {
public:
  explicit IoUringRouterClient(ChunkStore &store);
  ~IoUringRouterClient();

  bool connect(const std::string &host, uint16_t port);
  void disconnect();

private:
  void onPublish(const std::string &topic,
                 std::shared_ptr<std::vector<uint8_t>> data);

  void publishChunkLoadedCompressed(
      int32_t cx, int32_t cy, int32_t cz,
      std::shared_ptr<std::vector<uint8_t>> palette);

  ChunkStore &store_;
  gtnh::net::RouterClient router_;
};
