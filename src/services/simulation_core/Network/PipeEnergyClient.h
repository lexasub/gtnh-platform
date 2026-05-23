#pragma once
#include <memory>
#include <cstdint>
#include <vector>

namespace simcore {

class IoUringRouterClient;

class PipeEnergyClient {
public:
    explicit PipeEnergyClient(std::shared_ptr<IoUringRouterClient> router);

  void publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                           int32_t energy, int32_t capacity, int32_t max_input,
                           int32_t max_output, int32_t tier, int32_t energy_type,
                           bool is_source, bool is_sink,
                           const std::vector<uint64_t>& connected_nodes = {});

    void publishHeartbeat(uint64_t node_id, int32_t energy, int32_t capacity);

    void sendConsumeRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                            int32_t energy_type, int32_t needed);

 private:
     std::shared_ptr<IoUringRouterClient> router_;
 };

} // namespace simcore
