#pragma once
#include <cstdint>
#include <functional>

namespace simcore {

    struct CASResult {
        uint8_t status;   // 0 = OK, 1 = CONFLICT
        uint16_t block_id;
        uint8_t meta;
    };

    struct BlockData {
        uint16_t block_id;
        uint8_t meta;
        uint32_t mb_id;
    };

    class IBlockRepository {
    public:
        virtual ~IBlockRepository() = default;

        using SetBlockCASCallback = std::function<void(const CASResult&)>;
        virtual void setBlockCAS(int32_t x, int32_t y, int32_t z,
                                 uint16_t expected_block_id,
                                 uint16_t new_block_id,
                                 uint8_t meta,
                                 SetBlockCASCallback callback) = 0;

        using GetBlockCallback = std::function<void(const BlockData&)>;
        virtual void getBlock(int32_t x, int32_t y, int32_t z, GetBlockCallback callback) = 0;
    };

} // namespace simcore