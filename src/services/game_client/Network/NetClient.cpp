#include "NetClient.h"
#include "../World/ChunkView.h"
#include "gateway_generated.h"
#include "core_generated.h"

#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/tcp_connector.h>

#include <spdlog/spdlog.h>
#include <flatbuffers/verifier.h>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>

// =========================================================================
//  Construction / destruction
// =========================================================================

NetClient::NetClient()  = default;
NetClient::~NetClient() { Disconnect(); }

// =========================================================================
//  TCP connection helper (blocking, called once at startup)
// =========================================================================

int NetClient::tcp_connect(const char* host, uint16_t port) {
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* ai = nullptr;
    int rc = ::getaddrinfo(host, nullptr, &hints, &ai);
    if (rc != 0 || !ai) {
        spdlog::error("NetClient: getaddrinfo({}) failed: {}", host, gai_strerror(rc));
        return -1;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        spdlog::error("NetClient: socket() failed: {}", std::strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }

    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr   = reinterpret_cast<struct sockaddr_in*>(ai->ai_addr)->sin_addr;
    freeaddrinfo(ai);

    rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        spdlog::error("NetClient: connect() failed: {}", std::strerror(errno));
        ::close(fd);
        return -1;
    }

    struct pollfd pfd{fd, POLLOUT, 0};
    int poll_rc = ::poll(&pfd, 1, 5000);
    if (poll_rc <= 0 || !(pfd.revents & POLLOUT)) {
        spdlog::error("NetClient: connect timed out (poll rc={})", poll_rc);
        ::close(fd);
        return -1;
    }

    int so_err = 0;
    socklen_t errlen = sizeof(so_err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &errlen);
    if (so_err != 0) {
        spdlog::error("NetClient: connect SO_ERROR: {}", std::strerror(so_err));
        ::close(fd);
        return -1;
    }

    return fd;
}

// =========================================================================
//  IoUringConnection creation helpers
// =========================================================================

static gtnh::net::TagAllocator s_tag_alloc;

bool NetClient::start_ctrl_connection(int fd) {
    auto tags = s_tag_alloc.alloc();
    auto conn = std::make_unique<gtnh::net::IoUringConnection>(fd, "ctrl", tags);
    conn->on_message = [this](uint8_t type, const uint8_t* data, size_t len) {
        auto copy = std::make_shared<std::vector<uint8_t>>(data, data + len);
        std::lock_guard<std::mutex> lock(ctrl_mutex_);
        ctrl_queue_.push_back({type, std::move(copy)});
    };
    conn->on_closed = [this]() {
        connected_ctrl_ = false;
        spdlog::info("NetClient: ctrl connection closed");
    };
    if (!conn->start_reading()) {
        spdlog::error("NetClient: failed to start ctrl read loop");
        conn->close();
        return false;
    }
    ctrl_conn_ = std::move(conn);
    connected_ctrl_ = true;
    return true;
}

bool NetClient::start_bulk_connection(int fd) {
    auto tags = s_tag_alloc.alloc();
    auto conn = std::make_unique<gtnh::net::IoUringConnection>(fd, "bulk", tags);
    conn->on_message = [this](uint8_t type, const uint8_t* data, size_t len) {
        auto copy = std::make_shared<std::vector<uint8_t>>(data, data + len);
        std::lock_guard<std::mutex> lock(bulk_mutex_);
        bulk_queue_.push_back({type, std::move(copy)});
    };
    conn->on_closed = [this]() {
        connected_bulk_ = false;
        spdlog::info("NetClient: bulk connection closed");
        request_reconnect();
    };
    if (!conn->start_reading()) {
        spdlog::error("NetClient: failed to start bulk read loop");
        conn->close();
        return false;
    }
    bulk_conn_ = std::move(conn);
    connected_bulk_ = true;
    return true;
}

// =========================================================================
//  Connection
// =========================================================================

bool NetClient::Connect(const std::string& host, uint16_t ctrl_port, uint16_t bulk_port) {
    host_ = host;
    ctrl_port_ = ctrl_port;
    bulk_port_ = bulk_port;
    reconnect_attempts_ = 0;

    int ctrl_fd = tcp_connect(host.c_str(), ctrl_port);
    if (ctrl_fd < 0) return false;

    if (!start_ctrl_connection(ctrl_fd)) {
        ::close(ctrl_fd);
        return false;
    }
    spdlog::info("NetClient: ctrl connected to {}:{}", host, ctrl_port);

    int bulk_fd = tcp_connect(host.c_str(), bulk_port);
    if (bulk_fd < 0) {
        Disconnect();
        return false;
    }

    if (!start_bulk_connection(bulk_fd)) {
        ::close(bulk_fd);
        Disconnect();
        return false;
    }
    spdlog::info("NetClient: bulk connected to {}:{}", host, bulk_port);
    return true;
}

void NetClient::Disconnect() {
    connected_ctrl_ = false;
    connected_bulk_ = false;
    // Destroy connections — ~IoUringConnection joins poll threads
    ctrl_conn_.reset();
    bulk_conn_.reset();
    {
        std::lock_guard<std::mutex> lock(ctrl_mutex_);
        ctrl_queue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(bulk_mutex_);
        bulk_queue_.clear();
    }
    spdlog::info("NetClient: disconnected");
}

// =========================================================================
//  Reconnection
// =========================================================================

void NetClient::request_reconnect() {
    bool expected = false;
    if (!reconnecting_.compare_exchange_strong(expected, true))
        return;
    reconnect_requested_ = true;
}

void NetClient::do_reconnect() {
    spdlog::info("NetClient: bulk reconnecting...");

    // Destroy old connection (joins its poll thread)
    bulk_conn_.reset();
    connected_bulk_ = false;

    // Wait 100ms so the gateway's poll loop (50ms timeout) detects EOF on
    // the old connection. Otherwise the gateway rejects the new one because
    // client_bulk_->is_open() still returns true from the half-closed fd.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int attempt = 0; attempt < max_reconnect_attempts_; ++attempt) {
        int backoff = 1 << attempt;
        if (backoff > 30) backoff = 30;

        int fd = tcp_connect(host_.c_str(), bulk_port_);
        if (fd < 0) {
            spdlog::error("NetClient: bulk reconnect attempt {}/{} failed (backoff={}s)",
                          attempt + 1, max_reconnect_attempts_, backoff);
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            continue;
        }

        if (!start_bulk_connection(fd)) {
            ::close(fd);
            spdlog::error("NetClient: bulk reconnect init failed on attempt {}/{}",
                          attempt + 1, max_reconnect_attempts_);
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            continue;
        }

        spdlog::info("NetClient: bulk reconnected successfully on attempt {}/{}",
                     attempt + 1, max_reconnect_attempts_);
        reconnect_attempts_ = attempt + 1;
        reconnecting_ = false;
        return;
    }

    spdlog::warn("NetClient: bulk reconnect failed after {} attempts", max_reconnect_attempts_);
    connected_bulk_ = false;
    reconnecting_ = false;
}

// =========================================================================
//  Poll — drain message queues on game thread
// =========================================================================

void NetClient::drain_queue(std::deque<QueuedMessage>& queue, bool is_bulk) {
    std::deque<QueuedMessage> local;
    {
        std::lock_guard<std::mutex> lock(is_bulk ? bulk_mutex_ : ctrl_mutex_);
        local.swap(queue);
    }
    for (auto& msg : local) {
        if (is_bulk)
            OnBulkMessage(msg.type, std::move(msg.data));
        else
            OnMessage(msg.type, std::move(msg.data));
    }
}

void NetClient::Poll() {
    drain_queue(ctrl_queue_, false);
    drain_queue(bulk_queue_, true);

    if (reconnect_requested_.exchange(false)) {
        do_reconnect();
    }
}

// =========================================================================
//  Inbound message dispatch
// =========================================================================

// Ctrl connection: small messages (acks, inventory)
void NetClient::OnMessage(uint8_t msg_type,
                           std::shared_ptr<std::vector<uint8_t>> data) {
    const uint8_t* payload = data->data();
    size_t plen = data->size();

    switch (msg_type) {
        case GatewayMsg::kBlockAck:
            ProcessBlockAck(std::move(data));
            break;
        case GatewayMsg::kInventoryUpdate:
            if (onInventoryUpdate_)
                onInventoryUpdate_(data);
            break;
        case GatewayMsg::kBlockEntityUpdate:
            if (onBlockEntityUpdate_)
                onBlockEntityUpdate_(data);
            break;
        case GatewayMsg::kCraftResponse: {
            flatbuffers::Verifier v(payload, plen);
            if (!v.VerifyBuffer<Protocol::CraftResponse>(nullptr)) {
                return;
            }
            auto resp = flatbuffers::GetRoot<Protocol::CraftResponse>(payload);
            spdlog::info("CraftResponse: success={} error={}",
                         resp->success(), resp->error()->c_str());
            if (!onCraftResponse_) {
                return;
            }
            auto* r = resp->result();

            std::array<ItemStack, 9> grid{};
            if (auto* fbGrid = resp->grid()) {
                for (uint16_t i = 0; i < 9 && i < fbGrid->size(); ++i) {
                    auto* gs = fbGrid->Get(i);
                    if (gs) {
                        grid[i] = ItemStack{
                            static_cast<uint16_t>(gs->item_id()),
                            static_cast<uint8_t>(gs->count()),
                            static_cast<uint16_t>(gs->meta())
                        };
                    }
                }
            }

            onCraftResponse_(
                resp->success(),
                r ? static_cast<uint16_t>(r->item_id()) : 0,
                r ? static_cast<uint8_t>(r->count()) : 0,
                r ? static_cast<uint16_t>(r->meta()) : 0,
                resp->error() ? resp->error()->str() : "",
                grid
            );
            return;
        }
        case GatewayMsg::kRecipeCompleted:
            if (onRecipeCompleted_)
                onRecipeCompleted_(data);
            return;
        case GatewayMsg::kToolActionResp: {
            flatbuffers::Verifier v(payload, plen);
            if (!v.VerifyBuffer<Protocol::ToolActionResp>(nullptr)) {
                spdlog::warn("NetClient: invalid ToolActionResp buffer");
                return;
            }
            auto resp = flatbuffers::GetRoot<Protocol::ToolActionResp>(payload);
            if (onToolActionResp_) {
                std::vector<uint8_t> roles;
                if (auto* r = resp->all_roles()) {
                    roles.assign(r->begin(), r->end());
                }
                onToolActionResp_(resp->success(), resp->new_side_role(), roles);
            }
            return;
        }
        case GatewayMsg::kSetMachineSlotResp: {
            flatbuffers::Verifier v(payload, plen);
            if (!v.VerifyBuffer<Protocol::SetMachineSlotResp>(nullptr)) {
                spdlog::warn("NetClient: invalid SetMachineSlotResp buffer");
                return;
            }
            auto resp = flatbuffers::GetRoot<Protocol::SetMachineSlotResp>(payload);
            spdlog::info("SetMachineSlotResp: success={} slot={} pos({} {} {})",
                         resp->success(),
                         resp->slot_idx(),
                         resp->pos() ? resp->pos()->x() : 0,
                         resp->pos() ? resp->pos()->y() : 0,
                         resp->pos() ? resp->pos()->z() : 0);
            if (!onSetMachineSlotResp_) {
                return;
            }
            BlockPos pos;
            if (resp->pos()) {
                pos.x = resp->pos()->x();
                pos.y = resp->pos()->y();
                pos.z = resp->pos()->z();
            }
            onSetMachineSlotResp_(
                pos,
                resp->slot_idx(),
                resp->success(),
                resp->error() ? resp->error()->c_str() : "",
                resp->item() ? ItemStack{resp->item()->item_id(), resp->item()->count(), resp->item()->meta()} : ItemStack{0, 0, 0}
            );
            return;
        }
        case GatewayMsg::kQuestProgressUpdate:
        case GatewayMsg::kQuestUnlockNotification:
        case GatewayMsg::kQuestCompletedNotification:
            if (onQuestUpdate_)
                onQuestUpdate_(msg_type, data);
            return;
        case GatewayMsg::kChestOpenResp: {
            flatbuffers::Verifier v(payload, plen);
            if (!v.VerifyBuffer<Protocol::ChestOpenResp>(nullptr)) {
                spdlog::warn("NetClient: invalid ChestOpenResp buffer");
                return;
            }
            auto resp = flatbuffers::GetRoot<Protocol::ChestOpenResp>(payload);
            if (!onChestOpenResp_) return;
            BlockPos pos{};
            if (resp->pos()) {
                pos.x = resp->pos()->x();
                pos.y = resp->pos()->y();
                pos.z = resp->pos()->z();
            }
            std::vector<ItemStack> slots;
            if (auto* fbSlots = resp->slots()) {
                slots.reserve(fbSlots->size());
                for (flatbuffers::uoffset_t i = 0; i < fbSlots->size(); ++i) {
                    auto* s = fbSlots->Get(i);
                    if (s) slots.push_back(ItemStack{static_cast<uint16_t>(s->item_id()), s->count(), static_cast<uint16_t>(s->meta())});
                }
            }
            onChestOpenResp_(pos, true, slots);
            return;
        }
        default:
            spdlog::trace("NetClient: ctrl unknown msg_type={}", msg_type);
            break;
    }
}

// Bulk connection: large messages (chunks, block updates, entities)
void NetClient::OnBulkMessage(uint8_t msg_type,
                               std::shared_ptr<std::vector<uint8_t>> data) {
    switch (msg_type) {
        case GatewayMsg::kBlockUpdate:
            ProcessBlockUpdate(std::move(data));
            break;
        case GatewayMsg::kCompressedChunkData:
            ProcessKCompressedChunkData(std::move(data));
            break;
        default:
            spdlog::trace("NetClient: bulk unknown msg_type={}", msg_type);
            break;
    }
}

// =========================================================================
//  Chunk snapshot handler
// =========================================================================

bool NetClient::ProcessKCompressedChunkData(std::shared_ptr<std::vector<uint8_t>> data) {
    auto payload = data->data();
    auto size = data->size();

    flatbuffers::Verifier verifier(payload, size);
    if (!verifier.VerifyBuffer<Protocol::CompressedChunkData>(nullptr)) {
        spdlog::warn("NetClient: invalid CompressedChunkData buffer");
        return true;
    }

    auto compressed = flatbuffers::GetRoot<Protocol::CompressedChunkData>(payload);
    auto coord = compressed->coord();
    if (!coord) {
        spdlog::error("NetClient: CompressedChunkData has no coord");
        return true;
    }

    auto palette = compressed->palette_data();
    if (!palette || palette->size() == 0) {
        spdlog::error("NetClient: empty palette_data in CompressedChunkData");
        return true;
    }

    spdlog::info("Client: got chunk ({},{},{}), fb_size={}, palette_size={}",
                 coord->x(), coord->y(), coord->z(), size, palette->size());

    auto palette_copy = std::make_shared<std::vector<uint8_t>>(
        palette->data(), palette->data() + palette->size()
    );
    auto view = std::make_shared<ChunkView>(std::move(palette_copy));

    ChunkCoord cc{coord->x(), coord->y(), coord->z()};
    OnChunkData(view, cc);
    return false;
}

void NetClient::OnChunkData(std::shared_ptr<ChunkView> chunk,
                             const ChunkCoord& coord) {
    if (onChunkReceived_)
        onChunkReceived_(std::move(chunk), coord);
}

// =========================================================================
//  Block update handler
// =========================================================================

void NetClient::ProcessBlockUpdate(std::shared_ptr<std::vector<uint8_t>> data) {
    auto payload = data->data();
    auto size = data->size();
    flatbuffers::Verifier verifier(payload, size);
    if (!verifier.VerifyBuffer<Protocol::BlockChangedEvent>(nullptr)) {
        spdlog::warn("NetClient: invalid BlockChangedEvent buffer");
        return;
    }

    auto event = flatbuffers::GetRoot<Protocol::BlockChangedEvent>(payload);
    auto pos = event->pos();
    if (!pos) {
        spdlog::error("NetClient: BlockChangedEvent has no pos");
        return;
    }

    BlockPos bp{pos->x(), pos->y(), pos->z()};
    if (onBlockUpdate_)
        onBlockUpdate_(bp, event->block_id(), event->meta(), event->mb_id());
}

// =========================================================================
//  Block ack handler
// =========================================================================

void NetClient::ProcessBlockAck(std::shared_ptr<std::vector<uint8_t>> data) {
    auto payload = data->data();
    auto size = data->size();
    flatbuffers::Verifier verifier(payload, size);
    if (!verifier.VerifyBuffer<Protocol::BlockAck>(nullptr)) {
        spdlog::warn("NetClient: invalid BlockAck buffer");
        return;
    }

    auto ack = flatbuffers::GetRoot<Protocol::BlockAck>(payload);
    auto pos = ack->pos();
    if (!pos) {
        spdlog::error("NetClient: BlockAck has no pos");
        return;
    }

    if (onBlockAck_)
        onBlockAck_(BlockPos{pos->x(), pos->y(), pos->z()},
                    static_cast<uint8_t>(ack->status()),
                    ack->block_id(), ack->meta());
}

// =========================================================================
//  Outbound messages — all go through ctrl connection
// =========================================================================

void NetClient::EnqueueWrite(uint8_t msg_type, const void* data, size_t size) {
    if (!ctrl_conn_ || !connected_ctrl_) return;
    ctrl_conn_->send(msg_type, static_cast<const uint8_t*>(data), size);
}

void NetClient::RequestChunk(const ChunkCoord& coord) {
    if (!ctrl_conn_ || !connected_ctrl_) return;

    flatbuffers::FlatBufferBuilder builder(64);
    auto pos = Protocol::Vec3i(coord.x, coord.y, coord.z);
    auto action = Protocol::CreatePlayerAction(
        builder, 0, Protocol::PlayerActionType::PlayerActionType_CHUNK_REQUEST,
        &pos, 0, 0);
    builder.Finish(action);

    EnqueueWrite(GatewayMsg::kPlayerAction, builder.GetBufferPointer(),
                 builder.GetSize());
}

void NetClient::SendPlayerAction(uint64_t player_id,
                                  Protocol::PlayerActionType action,
                                  int32_t x, int32_t y, int32_t z,
                                  uint16_t item_id, uint8_t count) {
    if (!ctrl_conn_ || !connected_ctrl_) return;

    flatbuffers::FlatBufferBuilder builder(64);
    auto pos = Protocol::Vec3i(x, y, z);
    auto act = Protocol::CreatePlayerAction(builder, player_id, action, &pos, 0,
                                            count, item_id);
    builder.Finish(act);

    EnqueueWrite(GatewayMsg::kPlayerAction, builder.GetBufferPointer(),
                 builder.GetSize());
}

void NetClient::SendBlockAction(Protocol::PlayerActionType action,
                                  int32_t x, int32_t y, int32_t z,
                                  uint16_t currentBlockID, uint16_t block_id,
                                  uint8_t /*face*/, uint64_t player_id) {
    if (!ctrl_conn_ || !connected_ctrl_) return;

    flatbuffers::FlatBufferBuilder builder(64);
    auto pos = Protocol::Vec3i(x, y, z);
    auto act = Protocol::CreateSetBlockAction(builder, player_id, action, &pos,
                                              currentBlockID, block_id);
    builder.Finish(act);

    EnqueueWrite(GatewayMsg::kSetBlockAction, builder.GetBufferPointer(),
                 builder.GetSize());
}

void NetClient::SendCraftRequest(uint64_t player_id, const BlockPos& pos,
                                 const std::array<ItemStack, 9>& slots) {
    if (!ctrl_conn_ || !connected_ctrl_) return;
    flatbuffers::FlatBufferBuilder builder(128);
    auto posVec = Protocol::Vec3i(pos.x, pos.y, pos.z);
    std::vector<Protocol::ItemStack> slotVec;
    slotVec.reserve(9);
    for (const auto& s : slots) {
        slotVec.push_back(Protocol::ItemStack(s.item_id, s.count, s.meta));
    }
    auto slotsOffset = builder.CreateVectorOfStructs(slotVec);
    auto req = Protocol::CreateCraftRequest(builder, player_id, &posVec, slotsOffset);
    builder.Finish(req);
    EnqueueWrite(GatewayMsg::kCraftRequest, builder.GetBufferPointer(), builder.GetSize());
}

void NetClient::SendSetMachineSlot(uint64_t player_id, const BlockPos& pos,
                                    uint16_t slot_index, uint16_t item_id,
                                    uint8_t count, uint16_t meta,
                                    uint8_t player_slot) {
    if (!ctrl_conn_ || !connected_ctrl_) return;
    flatbuffers::FlatBufferBuilder builder(64);
    auto posVec = Protocol::Vec3i(pos.x, pos.y, pos.z);
    auto req = Protocol::CreateSetMachineSlotReq(builder, player_id, &posVec,
                                                  slot_index, item_id, count, meta,
                                                  player_slot);
    builder.Finish(req);
    EnqueueWrite(GatewayMsg::kSetMachineSlot, builder.GetBufferPointer(), builder.GetSize());
}

void NetClient::SendChestOpen(uint64_t player_id, const BlockPos& pos) {
    if (!ctrl_conn_ || !connected_ctrl_) return;
    flatbuffers::FlatBufferBuilder builder(64);
    auto posVec = Protocol::Vec3i(pos.x, pos.y, pos.z);
    auto req = Protocol::CreateChestOpenReq(builder, player_id, &posVec);
    builder.Finish(req);
    EnqueueWrite(GatewayMsg::kChestOpenReq, builder.GetBufferPointer(), builder.GetSize());
}

void NetClient::SendToolAction(uint64_t player_id, Protocol::ToolActionType action,
                                int32_t x, int32_t y, int32_t z, uint8_t face,
                                uint16_t item_id) {
    if (!ctrl_conn_ || !connected_ctrl_) return;
    flatbuffers::FlatBufferBuilder builder(64);
    auto pos = Protocol::Vec3i(x, y, z);
    auto act = Protocol::CreateToolAction(builder, player_id, action, &pos, face, item_id);
    builder.Finish(act);
    EnqueueWrite(GatewayMsg::kToolAction, builder.GetBufferPointer(), builder.GetSize());
}

void NetClient::SendInventoryAction(uint64_t player_id, uint8_t action_type,
                                    uint8_t source_slot, uint8_t target_slot, uint8_t count) {
    if (!ctrl_conn_ || !connected_ctrl_) return;
    flatbuffers::FlatBufferBuilder fbb(64);
    auto act = Protocol::CreateInventoryAction(fbb, player_id, action_type,
                                                source_slot, target_slot, count, 0);
    fbb.Finish(act);
    EnqueueWrite(GatewayMsg::kInventoryAction, fbb.GetBufferPointer(), fbb.GetSize());
}
