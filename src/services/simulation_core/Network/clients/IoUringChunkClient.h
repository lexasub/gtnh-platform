#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>

#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/tcp_connector.h>
#include <gtnh/net/types.h>

namespace simcore {

class IoUringChunkClient {
public:
    IoUringChunkClient();
    ~IoUringChunkClient();

    IoUringChunkClient(const IoUringChunkClient&) = delete;
    IoUringChunkClient& operator=(const IoUringChunkClient&) = delete;
    IoUringChunkClient(IoUringChunkClient&&) = delete;
    IoUringChunkClient& operator=(IoUringChunkClient&&) = delete;

    bool Connect(const std::string& host, uint16_t port);
    void Disconnect();
    bool IsConnected() const;

    using SetBlockCallback = std::function<void(bool success)>;
    void SetBlock(int32_t x, int32_t y, int32_t z, uint16_t block_id, uint8_t meta, SetBlockCallback callback);

    struct CASResult {
        uint8_t status;   // 0 = OK, 1 = CONFLICT
        uint16_t block_id;
        uint8_t meta;
    };
    using SetBlockCASCallback = std::function<void(const CASResult& result)>;
    void SetBlockCAS(int32_t x, int32_t y, int32_t z, uint16_t expected_block_id, uint16_t new_block_id, uint8_t meta, SetBlockCASCallback callback);

    struct BlockData {
        uint16_t block_id;
        uint8_t meta;
        uint32_t mb_id;
    };
    using GetBlockCallback = std::function<void(const BlockData& block)>;
    void GetBlock(int32_t x, int32_t y, int32_t z, GetBlockCallback callback);

private:
    void onRead(uint8_t msg_type, const uint8_t* data, size_t len);

    gtnh::net::TagAllocator tag_allocator_;
    gtnh::net::ConnectionTags tags_;
    std::unique_ptr<gtnh::net::IoUringConnection> conn_;
    int fd_ = -1;
    bool connected_ = false;
    bool stopped_ = false;
    std::string host_;
    uint16_t port_ = 0;

    std::atomic<uint32_t> next_req_id_{1};
    std::mutex pending_mutex_;
    std::unordered_map<uint32_t, std::function<void(const std::vector<uint8_t>&)>> pending_callbacks_;
};

} // namespace simcore
