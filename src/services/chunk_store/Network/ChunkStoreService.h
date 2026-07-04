#pragma once

#include "chunkstore_generated.h"
#include <array>
#include <asio.hpp>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

class ServerWorld;

class ChunkStoreService {
public:
  ChunkStoreService(ServerWorld &world, uint16_t port);
  ~ChunkStoreService();

  void start(); // запускает asio io_context в пуле потоков
  void stop();

private:
  void doAccept();

  void doReadSize(std::shared_ptr<asio::ip::tcp::socket> socket);

  void onReadPayload(std::error_code ec,
                     std::shared_ptr<asio::ip::tcp::socket> socket,
                     std::shared_ptr<std::vector<unsigned char>> payload);

  void onSizeRead(std::error_code ec,
                  std::shared_ptr<asio::ip::tcp::socket> socket,
                  std::shared_ptr<std::array<uint8_t, 4>> size_buf);
  void doReadPayload(std::shared_ptr<asio::ip::tcp::socket> socket,
                     uint32_t msg_size);
  void onPayloadRead(std::error_code ec,
                     std::shared_ptr<asio::ip::tcp::socket> socket,
                     std::shared_ptr<std::vector<uint8_t>> payload);

  // Обработчики запросов (теперь асинхронные)
  void handleGetBlock(std::shared_ptr<asio::ip::tcp::socket> socket,
                      uint32_t req_id, const Protocol::ChunkStoreMessage *req);
  void handleSetBlock(std::shared_ptr<asio::ip::tcp::socket> socket,
                      uint32_t req_id, const Protocol::ChunkStoreMessage *req);
  void handleGetChunk(std::shared_ptr<asio::ip::tcp::socket> socket,
                      uint32_t req_id, const Protocol::ChunkStoreMessage *req);
  void handleSaveChunk(std::shared_ptr<asio::ip::tcp::socket> socket,
                       uint32_t req_id, const Protocol::ChunkStoreMessage *req);
  void handleSetBlockCAS(std::shared_ptr<asio::ip::tcp::socket> socket,
                         uint32_t req_id,
                         const Protocol::ChunkStoreMessage *req);

  void EnqueueWrite(std::shared_ptr<asio::ip::tcp::socket> socket,
                    const flatbuffers::FlatBufferBuilder &fb);
  void DoWrite();

  void sendResponse(std::shared_ptr<asio::ip::tcp::socket> socket,
                    const flatbuffers::FlatBufferBuilder &fb);

  void sendError(std::shared_ptr<asio::ip::tcp::socket> socket, uint32_t req_id,
                 const std::string &msg);

  ServerWorld &world_;
  asio::io_context io_context_;
  asio::strand<asio::io_context::executor_type> write_strand_;
  asio::ip::tcp::acceptor acceptor_;
  std::vector<std::thread> workers_;
  std::atomic<bool> running_{false};

  struct WriteOp {
    std::shared_ptr<std::vector<uint8_t>> frame;
    std::shared_ptr<asio::ip::tcp::socket> socket;
  };
  std::deque<WriteOp> write_queue_;
};
