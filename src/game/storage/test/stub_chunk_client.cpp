// A link-time stub of simcore::IoUringChunkClient, used only by
// test_chunk_store_repository_stub.cpp.
//
// WHY THIS FILE EXISTS
// --------------------
// ChunkStoreRepository depends on the CONCRETE class IoUringChunkClient, not
// on an interface:
//
//   explicit ChunkStoreRepository(std::shared_ptr<IoUringChunkClient> client);
//
// and IoUringChunkClient has no virtual methods, so it cannot be subclassed
// into a double. This TU therefore DEFINES the handful of member functions
// that ChunkStoreRepository.cpp calls, so the linker resolves them here
// instead of pulling in src/apps/simcore/Network/clients/IoUringChunkClient.cpp
// (which is deliberately NOT added to this test target — that would be a
// duplicate-symbol clash).
//
// The real IoUringChunkClient.cpp is covered separately, end-to-end through
// the real repository, by test_chunk_store_repository.cpp, which links the
// real client. Between the two, every line of ChunkStoreRepository.cpp runs
// against production code.
//
// CONTRACT FIDELITY
// -----------------
// The stub reproduces the real client's *unconnected* semantics exactly
// (mirrored from IoUringChunkClient.cpp lines 103-107 / 141-145):
//
//   SetBlockCAS while disconnected -> callback(CASResult{1, 0, 0})   // CONFLICT
//   GetBlock    while disconnected -> callback(BlockData{0, 0, 0})
//
// and adds a scriptable connected mode so the forwarding and the
// CASResult/BlockData conversion can be observed. It is a TEST DOUBLE, not a
// model of the wire protocol: it never opens a socket, never spawns a thread
// and never touches the network.

#include "stub_chunk_client.h"

#include <apps/simcore/Network/clients/IoUringChunkClient.h>

#include <cstdint>

namespace simcore {

StubChunkClientControl g_stub;

IoUringChunkClient::IoUringChunkClient() : tags_(tag_allocator_.alloc()) {}

IoUringChunkClient::~IoUringChunkClient() = default;

bool IoUringChunkClient::IsConnected() const { return g_stub.connected; }

void IoUringChunkClient::SetBlockCAS(int32_t x, int32_t y, int32_t z,
                                     uint16_t expected_block_id,
                                     uint16_t new_block_id, uint8_t meta,
                                     SetBlockCASCallback callback) {
  g_stub.cas_calls.push_back(
      StubChunkClientControl::CasCall{x, y, z, expected_block_id, new_block_id,
                                      meta});
  if (!g_stub.connected) {
    // Byte-for-byte what the real client does when not connected
    // (IoUringChunkClient.cpp:104-107). Reached only if a caller bypasses the
    // repository's own IsConnected() guard and calls the client directly.
    if (callback) callback(CASResult{1, 0, 0});
    return;
  }
  if (callback) callback(CASResult{g_stub.reply_status, g_stub.reply_block_id,
                                  g_stub.reply_meta});
}

void IoUringChunkClient::GetBlock(int32_t x, int32_t y, int32_t z,
                                  GetBlockCallback callback) {
  g_stub.get_calls.push_back(StubChunkClientControl::GetCall{x, y, z});
  if (!g_stub.connected) {
    // IoUringChunkClient.cpp:142-145.
    if (callback) callback(BlockData{0, 0, 0});
    return;
  }
  if (callback) {
    callback(BlockData{g_stub.reply_block_id, g_stub.reply_meta,
                       g_stub.reply_mb_id});
  }
}

} // namespace simcore
