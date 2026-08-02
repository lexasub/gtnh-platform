#pragma once
#include "Network/ITopicHandler.h"
#include "Network/clients/IoUringChunkClient.h"
#include <memory>

namespace simcore {

class CableExplosionHandler : public ITopicHandler {
public:
  explicit CableExplosionHandler(std::shared_ptr<IoUringChunkClient> chunkClient);

  void handle(const std::vector<uint8_t> &data) override;

private:
  std::shared_ptr<IoUringChunkClient> chunkClient_;
};

} // namespace simcore
