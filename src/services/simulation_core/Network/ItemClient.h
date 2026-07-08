#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace simcore {

class IoUringRouterClient;

class ItemClient {
public:
  explicit ItemClient(std::shared_ptr<IoUringRouterClient> router);

  void publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                         const std::vector<uint16_t>& item_ids,
                         const std::vector<uint8_t>& item_counts,
                         int32_t capacity, bool is_source, bool is_sink,
                         const std::vector<uint64_t>& connected_nodes = {});

  void sendTransferRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                           uint16_t item_id, uint8_t count, int32_t tier);

private:
  std::shared_ptr<IoUringRouterClient> router_;
};

} // namespace simcore
