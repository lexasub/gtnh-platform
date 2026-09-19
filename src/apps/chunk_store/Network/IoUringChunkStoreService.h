#pragma once

#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/server.h>

#include "chunkstore_generated.h"

#include <atomic>
#include <cstdint>
#include <flatbuffers/flatbuffers.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ChunkStore;

// io_uring-based TCP service that replaces the Asio ChunkStoreService.
// Listens on a port, accepts connections, handles GetBlock/SetBlock/
// SaveChunk/SetBlockCAS requests using the synchronous ChunkStore API.
class IoUringChunkStoreService {
public:
  explicit IoUringChunkStoreService(ChunkStore &store);
  ~IoUringChunkStoreService();

  bool listen(uint16_t port);
  void stop();

private:
  void onAccept(int client_fd);
  void onMessage(std::shared_ptr<gtnh::net::IoUringConnection> conn,
                 uint8_t msg_type, const uint8_t *data, size_t len);
  void onClosed(std::shared_ptr<gtnh::net::IoUringConnection> conn);

  // Request handlers
  void handleGetBlock(std::shared_ptr<gtnh::net::IoUringConnection> conn,
                      uint32_t req_id,
                      const Protocol::ChunkStoreMessage *req);
  void handleSetBlock(std::shared_ptr<gtnh::net::IoUringConnection> conn,
                      uint32_t req_id,
                      const Protocol::ChunkStoreMessage *req);
  void handleSaveChunk(std::shared_ptr<gtnh::net::IoUringConnection> conn,
                       uint32_t req_id,
                       const Protocol::ChunkStoreMessage *req);
  void handleSetBlockCAS(std::shared_ptr<gtnh::net::IoUringConnection> conn,
                         uint32_t req_id,
                         const Protocol::ChunkStoreMessage *req);

  void sendResponse(std::shared_ptr<gtnh::net::IoUringConnection> conn,
                    const flatbuffers::FlatBufferBuilder &fb);

  ChunkStore &store_;
  gtnh::net::TcpServer server_;
  gtnh::net::TagAllocator tagAlloc_;

  std::mutex connsMutex_;
  std::vector<std::shared_ptr<gtnh::net::IoUringConnection>> connections_;
  std::atomic<bool> running_{false};
};
