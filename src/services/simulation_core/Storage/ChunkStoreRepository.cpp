#include "Storage/ChunkStoreRepository.h"
#include "Network/clients/IoUringChunkClient.h"
#include <spdlog/spdlog.h>

namespace simcore {

    ChunkStoreRepository::ChunkStoreRepository(std::shared_ptr<IoUringChunkClient> client)
        : client_(std::move(client))
    {}

    void ChunkStoreRepository::setBlockCAS(int32_t x, int32_t y, int32_t z,
                                           uint16_t expected_block_id,
                                           uint16_t new_block_id,
                                           uint8_t meta,
                                           SetBlockCASCallback callback)
    {
        if (!client_ || !client_->IsConnected()) {
            spdlog::error("ChunkStoreRepository: client not connected");
            callback(CASResult{1, 0, 0}); // CONFLICT
            return;
        }

        client_->SetBlockCAS(x, y, z, expected_block_id, new_block_id, meta,
            [callback](const IoUringChunkClient::CASResult& result) {
                CASResult converted{result.status, result.block_id, result.meta};
                callback(converted);
            });
    }

    void ChunkStoreRepository::getBlock(int32_t x, int32_t y, int32_t z, GetBlockCallback callback)
    {
        if (!client_ || !client_->IsConnected()) {
            spdlog::error("ChunkStoreRepository: client not connected");
            callback(BlockData{0, 0, 0});
            return;
        }

        client_->GetBlock(x, y, z,
            [callback](const IoUringChunkClient::BlockData& data) {
                BlockData converted{data.block_id, data.meta, data.mb_id};
                callback(converted);
            });
    }

} // namespace simcore