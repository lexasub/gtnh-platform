#include <asio.hpp>

#include <spdlog/spdlog.h>
#include <csignal>
#include <atomic>
#include <memory>
#include <string>

#include "../../libs/libgtnh-common/metrics_util.h"

#include "EntityStateStorage.h"
#include "cache.h"
#include "Client/MessageRouterClient.h"
#include "Client/ChunkStoreClient.h"
#include "core_generated.h"
#include "entity_state_store_generated.h"

static std::atomic<bool> g_stop{false};
static asio::io_context* g_io = nullptr;

static void doReadFrame(std::shared_ptr<asio::ip::tcp::socket> socket, EntityStateStorage& storage);

static void doAccept(asio::ip::tcp::acceptor& acceptor, EntityStateStorage& storage) {
    acceptor.async_accept([&](asio::error_code ec, asio::ip::tcp::socket socket) {
        if (!ec) {
            spdlog::info("EntityStateService: new RPC connection");
            auto sock = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
            doReadFrame(sock, storage);
        } else if (ec != asio::error::operation_aborted) {
            spdlog::warn("EntityStateService: accept error: {}", ec.message());
        }
        if (!g_stop.load()) {
            doAccept(acceptor, storage);
        }
    });
}

static void doReadFrame(std::shared_ptr<asio::ip::tcp::socket> socket, EntityStateStorage& storage) {
    auto size_buf = std::make_shared<std::array<uint8_t, 4>>();
    asio::async_read(*socket, asio::buffer(*size_buf),
        [socket, &storage, size_buf](asio::error_code ec, size_t) {
            if (ec) {
                if (ec != asio::error::eof && ec != asio::error::connection_reset) {
                    spdlog::warn("EntityStateService RPC read error: {}", ec.message());
                }
                return;
            }

            uint32_t msg_size =
                (static_cast<uint32_t>((*size_buf)[0]) << 24) |
                (static_cast<uint32_t>((*size_buf)[1]) << 16) |
                (static_cast<uint32_t>((*size_buf)[2]) << 8) |
                static_cast<uint32_t>((*size_buf)[3]);

            if (msg_size < 4 || msg_size > 2 * 1024 * 1024) {
                spdlog::warn("EntityStateService: invalid frame size {}", msg_size);
                return;
            }

            auto payload = std::make_shared<std::vector<uint8_t>>(msg_size);
            asio::async_read(*socket, asio::buffer(*payload),
                [socket, &storage, payload](asio::error_code ec, size_t) {
                    if (ec) return;

                    flatbuffers::Verifier verifier(payload->data(), payload->size());
                    if (!Protocol::VerifyEntityStateStoreFrameBuffer(verifier)) {
                        spdlog::warn("EntityStateService: invalid FlatBuffer");
                        return;
                    }

                    auto* frame = flatbuffers::GetRoot<Protocol::EntityStateStoreFrame>(payload->data());
                    if (!frame || !frame->payload()) return;

                    if (frame->payload_type() != Protocol::EntityStateStorePayload_EntityStateStoreMessage) {
                        spdlog::warn("EntityStateService: unexpected payload type");
                        return;
                    }

                    auto* msg = frame->payload_as_EntityStateStoreMessage();
                    if (!msg) return;

                    uint32_t req_id = msg->req_id();

                    flatbuffers::FlatBufferBuilder fbb;
                    switch (msg->request_type()) {
                        case Protocol::EntityStateStoreRequest_GetEntityStateReq: {
                            auto* req = msg->request_as_GetEntityStateReq();
                            if (!req) break;

                            std::vector<uint8_t> stateData;
                            if (storage.LoadEntityState(req->dimension(), req->x(), req->y(), req->z(),
                                                        req->entity_type(), stateData)) {
                                auto stateVec = fbb.CreateVector(stateData);
                                auto resp = Protocol::CreateGetEntityStateResp(fbb, stateVec);
                                auto reply = Protocol::CreateEntityStateStoreReply(fbb, req_id,
                                    Protocol::EntityStateStoreResponse_GetEntityStateResp, resp.Union());
                                auto frameOut = Protocol::CreateEntityStateStoreFrame(fbb,
                                    Protocol::EntityStateStorePayload_EntityStateStoreReply, reply.Union());
                                fbb.Finish(frameOut);
                            } else {
                                auto stateVec = fbb.CreateVector(std::vector<uint8_t>{});
                                auto resp = Protocol::CreateGetEntityStateResp(fbb, stateVec);
                                auto reply = Protocol::CreateEntityStateStoreReply(fbb, req_id,
                                    Protocol::EntityStateStoreResponse_GetEntityStateResp, resp.Union());
                                auto frameOut = Protocol::CreateEntityStateStoreFrame(fbb,
                                    Protocol::EntityStateStorePayload_EntityStateStoreReply, reply.Union());
                                fbb.Finish(frameOut);
                            }
                            break;
                        }
                        case Protocol::EntityStateStoreRequest_SetEntityStateReq: {
                            auto* req = msg->request_as_SetEntityStateReq();
                            if (!req) break;

                            std::vector<uint8_t> stateData(req->state()->begin(), req->state()->end());
                            storage.SaveEntityState(req->dimension(), req->x(), req->y(), req->z(),
                                                    req->entity_type(), stateData);

                            auto resp = Protocol::CreateEntityStateAck(fbb, true);
                            auto reply = Protocol::CreateEntityStateStoreReply(fbb, req_id,
                                Protocol::EntityStateStoreResponse_EntityStateAck, resp.Union());
                            auto frameOut = Protocol::CreateEntityStateStoreFrame(fbb,
                                Protocol::EntityStateStorePayload_EntityStateStoreReply, reply.Union());
                            fbb.Finish(frameOut);
                            break;
                        }
                        default: {
                            spdlog::warn("EntityStateService: unknown request type {}", static_cast<int>(msg->request_type()));
                            auto errStr = fbb.CreateString("Unknown request type");
                            auto resp = Protocol::CreateErrorResp(fbb, errStr);
                            auto reply = Protocol::CreateEntityStateStoreReply(fbb, req_id,
                                Protocol::EntityStateStoreResponse_ErrorResp, resp.Union());
                            auto frameOut = Protocol::CreateEntityStateStoreFrame(fbb,
                                Protocol::EntityStateStorePayload_EntityStateStoreReply, reply.Union());
                            fbb.Finish(frameOut);
                            break;
                        }
                    }

                    uint32_t reply_size = fbb.GetSize();
                    auto out_frame = std::make_shared<std::vector<uint8_t>>(4 + reply_size);
                    (*out_frame)[0] = static_cast<uint8_t>((reply_size) >> 24);
                    (*out_frame)[1] = static_cast<uint8_t>((reply_size) >> 16);
                    (*out_frame)[2] = static_cast<uint8_t>((reply_size) >> 8);
                    (*out_frame)[3] = static_cast<uint8_t>(reply_size);
                    std::memcpy(out_frame->data() + 4, fbb.GetBufferPointer(), reply_size);

                    asio::async_write(*socket, asio::buffer(*out_frame),
                        [socket, &storage, out_frame](asio::error_code ec, size_t) {
                            if (ec) {
                                spdlog::warn("EntityStateService RPC write error: {}", ec.message());
                                return;
                            }
                            doReadFrame(socket, storage);
                        });
                });
        });
}

[[maybe_unused]] static void handleSignal(int sig) {
    spdlog::info("Signal {} received, shutting down...", sig);
    g_stop.store(true, std::memory_order_release);
    if (g_io) g_io->stop();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    gtnh::metrics::printVersionAndExit("EntityStateStore Service (entitystated)", argc, argv);

    gtnh::metrics::Collector metrics;
    metrics.install();

    std::string lmdb_path = "/tmp/lmdb";
    asio::io_context io_context;
    EntityStateStorage storage(lmdb_path, io_context, 1000);

    if (!storage.initialize()) {
        spdlog::error("Failed to initialize EntityStateStorage");
        return 1;
    }

    auto routerClient = std::make_shared<gtnh::entity_state_store::MessageRouterClient>(io_context);
    auto chunkstoreClient = std::make_shared<gtnh::entity_state_store::ChunkStoreClient>(io_context);

    routerClient->SetServiceName("entitystated");
    routerClient->OnMessage([&](const std::string& topic, const std::vector<uint8_t>& data) {
        if (topic == "entity.state.get") {
            auto req = flatbuffers::GetRoot<Protocol::GetEntityStateReq>(data.data());
            if (!req) {
                spdlog::warn("Invalid GetEntityStateReq received");
                return;
            }

            std::vector<uint8_t> stateData;
            if (storage.LoadEntityState(req->dimension(), req->x(), req->y(), req->z(),
                                       req->entity_type(), stateData)) {
                flatbuffers::FlatBufferBuilder fbb;
                auto stateVec = fbb.CreateVector(stateData);
                auto resp = Protocol::CreateGetEntityStateResp(fbb, stateVec);
                fbb.Finish(resp);

                routerClient->Publish("entity.state.response",
                    std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()));
            } else {
                flatbuffers::FlatBufferBuilder fbb;
                auto resp = Protocol::CreateGetEntityStateResp(fbb, fbb.CreateVector(std::vector<uint8_t>{}));
                fbb.Finish(resp);

                routerClient->Publish("entity.state.response",
                    std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()));
            }
        } else if (topic == "entity.state.set") {
            auto req = flatbuffers::GetRoot<Protocol::SetEntityStateReq>(data.data());
            if (!req) {
                spdlog::warn("Invalid SetEntityStateReq received");
                return;
            }

            std::vector<uint8_t> stateData(req->state()->begin(), req->state()->end());
            storage.SaveEntityState(req->dimension(), req->x(), req->y(), req->z(),
                                   req->entity_type(), stateData);

            flatbuffers::FlatBufferBuilder fbb;
            auto resp = Protocol::CreateEntityStateAck(fbb, true);
            fbb.Finish(resp);

            routerClient->Publish("entity.state.ack",
                std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()));
        } else if (topic == "world.blocks.changed") {
            spdlog::debug("Received block changed event");
        } else {
            spdlog::debug("Unhandled topic: {}", topic);
        }
    });

    routerClient->Subscribe("entity.state.get");
    routerClient->Subscribe("entity.state.set");
    routerClient->Subscribe("world.blocks.changed");

    // Configure endpoints (could come from config)
    std::string router_host = "127.0.0.1";
    uint16_t router_port = 4000;
    std::string chunkstore_host = "127.0.0.1";
    uint16_t chunkstore_port = 5001;

    // Register signal handlers
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    
    g_io = &io_context;

    routerClient->Connect(router_host, router_port);
    chunkstoreClient->Connect(chunkstore_host, chunkstore_port);

    asio::ip::tcp::acceptor tcp_acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 5200));
    spdlog::info("EntityStateService listening on TCP 5200 for direct RPC");
    doAccept(tcp_acceptor, storage);

    spdlog::info("EntityStateService running, waiting for messages...");
    
    // Run io_context in a loop that checks for signals
    while (!g_stop.load()) {
        // Check metrics first (lightweight atomic check)
        if (metrics.poll()) {
            metrics.printMetrics("EntityStateStore Service (entitystated)");
        }
        
        // Then process network I/O
        io_context.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    storage.shutdown();
    spdlog::info("EntityStateService shutdown complete");
    return 0;
}