#include "RouterClient.h"
#include "FrameCodec.h"
#include "core_generated.h"
#include "chunkstore_generated.h"
#include "../Storage/ChunkStore.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <common/coords/Coords.h>

enum MsgType : uint8_t {
    MsgSubscribe   = 0x01,
    MsgUnsubscribe = 0x02,
    MsgPublish     = 0x03,
    MsgRegister    = 0x04,
    MsgHeartbeat   = 0x05,
};

RouterClient::RouterClient(ChunkStore& store)
    : store_(store), socket_(io_context_), heartbeat_timer_(io_context_),
      reconnect_timer_(io_context_), write_strand_(io_context_.get_executor()) {}

RouterClient::~RouterClient() { stop(); }

void RouterClient::connect(const std::string& host, uint16_t port) {
    host_ = host; port_ = port;
    doConnect();
}

void RouterClient::run() { io_context_.run(); }

void RouterClient::stop() {
    std::error_code ec;
    heartbeat_timer_.cancel(ec);
    reconnect_timer_.cancel(ec);
    socket_.close(ec);
    io_context_.stop();
}

void RouterClient::doConnect() {
    std::error_code close_ec;
    socket_.close(close_ec);

    asio::ip::tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(host_, std::to_string(port_));
    asio::async_connect(socket_, endpoints,
        [this](std::error_code ec, asio::ip::tcp::endpoint) {
            if (ec) {
                spdlog::error("Router connect failed: {}", ec.message());
                scheduleReconnect();
                return;
            }
            spdlog::info("Connected to router {}:{}", host_, port_);
            reconnect_delay_ = 1;
            reconnect_pending_ = false;
            doRegister();
            subscribe("player.action");
            scheduleHeartbeat();
            doReadHeader();
        });
}

void RouterClient::doRegister() {
    const std::string name = "chunkstore";
    std::vector<std::string> topics = {"player.action"};
    std::vector<uint8_t> frame;
    FrameCodec::buildRegisterFrame(frame, name, topics);
    EnqueueWrite(std::make_shared<std::vector<uint8_t>>(std::move(frame)));
}

void RouterClient::subscribe(const std::string& topic) {
    std::vector<uint8_t> frame;
    FrameCodec::buildSubscribeFrame(frame, topic);
    EnqueueWrite(std::make_shared<std::vector<uint8_t>>(std::move(frame)));
    spdlog::debug("Subscribed to '{}'", topic);
}

void RouterClient::doReadHeader() {
    asio::async_read(socket_, asio::buffer(header_),
        [this](std::error_code ec, size_t) { onHeaderRead(ec); });
}

void RouterClient::onHeaderRead(std::error_code ec) {
    if (ec) {
        spdlog::warn("Router header read error: {} — reconnecting", ec.message());
        scheduleReconnect();
        return;
    }
    uint32_t payload_len = FrameCodec::readBE32(header_.data());
    uint8_t msg_type = header_[4];
    if (payload_len < 1) return;
    if (msg_type == MsgHeartbeat) {
        doReadHeader();
        return;
    }
    doReadPayload(payload_len - 1);
}

void RouterClient::doReadPayload(uint32_t len) {
    read_buf_.resize(len);
    asio::async_read(socket_, asio::buffer(read_buf_),
        [this](std::error_code ec, size_t) { onPayloadRead(ec); });
}

void RouterClient::onPayloadRead(std::error_code ec) {
    if (ec) {
        spdlog::warn("Router payload read error: {} — reconnecting", ec.message());
        scheduleReconnect();
        return;
    }
    onPublish(read_buf_.data(), read_buf_.size());
    doReadHeader();
}

void RouterClient::onPublish(const uint8_t* data, size_t len) {
    if (len < 2) return;
    uint16_t topic_len = (data[0] << 8) | data[1];
    if (static_cast<size_t>(2 + topic_len) > len) return;
    std::string topic(reinterpret_cast<const char*>(data+2), topic_len);
    const uint8_t* fb_data = data + 2 + topic_len;




    if (topic != "player.action") return;
    auto* action = flatbuffers::GetRoot<Protocol::PlayerAction>(fb_data);
    auto* pos = action->pos();
    auto act = action->action();
    // ChunkStore handles only CHUNK_REQUEST.
    // PLACE/BREAK go through SimulationCore (CAS protocol).
    if (act != Protocol::PlayerActionType::PlayerActionType_CHUNK_REQUEST) {
        return;
    }
    ChunkCoord coord{pos->x(), pos->y(), pos->z()};
    auto exec = socket_.get_executor();
    store_.AsyncGetChunk(coord,
                         [self = weak_from_this(), coord, exec = std::move(exec)]
                 (std::shared_ptr<std::vector<uint8_t>> palette) {
                             if (!palette || palette->empty()) return;
                             asio::post(exec, [self, coord, palette = std::move(palette)] {
                                  if (auto s = self.lock())
                                      s->publishChunkLoadedCompressed(coord.x, coord.y, coord.z, std::move(palette));
                             });
                         });
}

void RouterClient::publishBlockChanged(int32_t x, int32_t y, int32_t z,
                                        uint16_t block_id, uint8_t meta, uint32_t mb_id) {
    flatbuffers::FlatBufferBuilder fb(64);
    Protocol::Vec3i pos(x,y,z);
    auto event = Protocol::CreateBlockChangedEvent(fb, &pos, block_id, meta, mb_id);
    fb.Finish(event);
    std::vector<uint8_t> frame;
    FrameCodec::buildPublishFrame(frame, "world.blocks.changed", fb.GetBufferPointer(), fb.GetSize());
    EnqueueWrite(std::make_shared<std::vector<uint8_t>>(std::move(frame)));
}

void RouterClient::publishChunkLoadedCompressed(int32_t cx, int32_t cy, int32_t cz,
                                                 std::shared_ptr<std::vector<uint8_t>> palette) {
    flatbuffers::FlatBufferBuilder fb(palette->size() + 128);
    Protocol::Vec3i coord(cx, cy, cz);
    auto palette_fb = fb.CreateVector(palette->data(), palette->size());
    auto compressed = Protocol::CreateCompressedChunkData(fb, &coord, palette_fb);
    fb.Finish(compressed);

    std::vector<uint8_t> frame;
    FrameCodec::buildPublishFrame(frame, "world.chunk.loaded.compressed",
                                   fb.GetBufferPointer(), fb.GetSize());
    EnqueueWrite(std::make_shared<std::vector<uint8_t>>(std::move(frame)));
}

void RouterClient::scheduleHeartbeat() {
    heartbeat_timer_.expires_after(std::chrono::seconds(HEARTBEAT_INTERVAL_SEC));
    heartbeat_timer_.async_wait([this](std::error_code ec) { doHeartbeat(ec); });
}

void RouterClient::doHeartbeat(std::error_code ec) {
    // Fixed sigsegv: added missing return
    if (ec) return;
    uint8_t frame[5];
    FrameCodec::writeBE32(frame, 1);
    frame[4] = MsgHeartbeat;
    EnqueueWrite(std::make_shared<std::vector<uint8_t>>(frame, frame + 5));
    scheduleHeartbeat();
}

void RouterClient::scheduleReconnect() {
    // Don't stack reconnections: if reconnect is already pending, skip
    if (reconnect_pending_) return;
    reconnect_pending_ = true;

    spdlog::warn("Router disconnected — reconnecting in {}s", reconnect_delay_);

    std::error_code ec;
    heartbeat_timer_.cancel(ec);

    // Clear pending writes BEFORE closing socket: inflight async_write handlers
    // will still fire but DoWrite sees empty queue and write_in_progress_=false.
    write_queue_.clear();
    write_in_progress_ = false;
    socket_.close(ec);

    reconnect_timer_.expires_after(std::chrono::seconds(reconnect_delay_));
    reconnect_timer_.async_wait(
        [self = shared_from_this()](std::error_code ec) {
            if (ec) { self->reconnect_pending_ = false; return; }
            self->reconnect_pending_ = false;
            self->doConnect();
        });

    reconnect_delay_ = std::min(reconnect_delay_ * 2, 30);
}

// Async write queue implementation
void RouterClient::EnqueueWrite(std::shared_ptr<std::vector<uint8_t>> frame) {
    asio::post(write_strand_, [this, frame = std::move(frame)]() mutable {
        bool was_empty = write_queue_.empty();
        write_queue_.push_back({std::move(frame)});
        if (was_empty) {
            DoWrite();
        }
    });
}

void RouterClient::DoWrite() {
    if (write_queue_.empty() || write_in_progress_) return;
    if (!socket_.is_open()) {
        write_queue_.clear();
        return;
    }
    
    write_in_progress_ = true;
    auto op = std::move(write_queue_.front());
    write_queue_.pop_front();
    auto buf = asio::buffer(*op);
    asio::async_write(socket_, buf,
        asio::bind_executor(write_strand_, [this, op = std::move(op)](const asio::error_code& ec, std::size_t bytes) {
            write_in_progress_ = false;
            if (ec && ec != asio::error::operation_aborted) {
                spdlog::error("RouterClient write error: {} ({} bytes)", ec.message(), bytes);
            }
            DoWrite();
        }));
}