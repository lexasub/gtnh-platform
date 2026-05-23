#pragma once
#include <memory>
#include "IBlockRepository.h"

namespace simcore {

    class IoUringChunkClient;

    class ChunkStoreRepository : public IBlockRepository {
    public:
        explicit ChunkStoreRepository(std::shared_ptr<IoUringChunkClient> client);

        void setBlockCAS(int32_t x, int32_t y, int32_t z,
                         uint16_t expected_block_id,
                         uint16_t new_block_id,
                         uint8_t meta,
                         SetBlockCASCallback callback) override;

        void getBlock(int32_t x, int32_t y, int32_t z, GetBlockCallback callback) override;

    private:
        std::shared_ptr<IoUringChunkClient> client_;
    };

} // namespace simcore