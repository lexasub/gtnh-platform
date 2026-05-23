#pragma once

#include "../World/ServerWorld.h"
#include "chunkstore_generated.h"
#include <asio.hpp>
#include <atomic>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>


// RouterClient connects to the Go MessageRouter (TCP pub/sub bus).
//
// Wire frame format (matching Go router):
//   [4 bytes: payload length, big-endian] [1 byte: message type] [payload]
//
// Message types:
//   0x01 Subscribe   — payload: [2 bytes topic len BE][topic]
//   0x03 Publish     — payload: [2 bytes topic len BE][topic][opaque data]
//   0x04 Register    — payload: [2 bytes name len BE][name][2 bytes ntopics BE][topic...]
//   0x05 Heartbeat   — payload: none
//
// Topics:
//   Subscribe: world.blocks.set, world.chunk.load
//   Publish:   world.blocks.changed, world.chunk.loaded

struct Chunk;

class RouterClient : public std::enable_shared_from_this<RouterClient> {
public:
    explicit RouterClient(ServerWorld& world);
    ~RouterClient();

    // Initiate connection to router. Non-blocking — returns immediately.
    void connect(const std::string& host, uint16_t port);

    // Runs the io_context (blocking). Call from a dedicated thread.
    void run();

    // Signals io_context to stop. Thread-safe.
    void stop();

private:
    // ASIO async chain
    void doConnect();
    void scheduleReconnect();
    void doRegister();
    void doReadHeader();
    void onHeaderRead(std::error_code ec);
    void doReadPayload(uint32_t len);
    void onPayloadRead(std::error_code ec);

    void scheduleHeartbeat();
    void doHeartbeat(std::error_code ec);

    // Incoming message dispatch
    void onPublish(const uint8_t* payload, size_t len);

    // Outgoing helpers
    void writeFrame(const uint8_t* data, size_t len);
    void subscribe(const std::string& topic);
    void publishBlockChanged(int32_t x, int32_t y, int32_t z,
                                 uint16_t block_id, uint8_t meta, uint32_t mb_id);
    void publishChunkLoadedCompressed(int32_t cx, int32_t cy, int32_t cz, const Chunk& chunk);

    // Async write queue
    void EnqueueWrite(std::shared_ptr<std::vector<uint8_t>> frame);
    void DoWrite();

    ServerWorld& world_;

    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer heartbeat_timer_;
    asio::steady_timer reconnect_timer_;
    asio::strand<asio::io_context::executor_type> write_strand_;
    bool write_in_progress_ = false;

    // Reusable read buffer for size + type header
    std::array<uint8_t, 5> header_;

    // Buffer for incoming frames
    std::vector<uint8_t> read_buf_;

    std::string host_;
    uint16_t port_ = 0;
    int reconnect_delay_ = 1;
    bool reconnect_pending_ = false;

    // Write queue for async operations
    std::deque<std::shared_ptr<std::vector<uint8_t>>> write_queue_;

    // Heartbeat every 20s; router times out at 60s
    static constexpr int HEARTBEAT_INTERVAL_SEC = 20;
};

