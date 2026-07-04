#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace simcore {

class IoUringRouterClient;

class FluidClient {
public:
  explicit FluidClient(std::shared_ptr<IoUringRouterClient> router);

  void publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                         uint32_t fluid_id, int32_t amount, int32_t capacity,
                         int32_t max_input, int32_t max_output, int32_t tier,
                         bool is_source, bool is_sink,
                         const std::vector<uint64_t> &connected_nodes = {});

  void sendFluidRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                        uint32_t fluid_id, int32_t amount);

private:
  std::shared_ptr<IoUringRouterClient> router_;
};

} // namespace simcore
