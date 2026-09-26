// A link-time stub of simcore::EntityStateStoreClient, used only by
// WorldContainerInventory_test.cpp.
//
// WHY THIS FILE EXISTS
// --------------------
// WorldContainerInventory's constructor takes a std::shared_ptr to the
// CONCRETE class EntityStateStoreClient:
//
//   WorldContainerInventory(entt::registry&, std::shared_ptr<EntityStateStoreClient>);
//
// and EntityStateStoreClient has no virtual methods, so it cannot be
// subclassed into a double. This TU therefore DEFINES the handful of member
// functions that WorldContainerInventory.cpp calls, so the linker resolves
// them here instead of pulling in
// src/apps/simcore/Network/clients/EntityStateStoreClient.cpp (which is
// deliberately NOT added to this test target — that would be a duplicate
// symbol clash, and it is only linked into the simcored executable).
//
// This is the same pattern as src/game/storage/test/stub_chunk_client.cpp.
//
// CONTRACT FIDELITY
// -----------------
// The stub reproduces the real client's UNCONNECTED semantics exactly
// (mirrored from EntityStateStoreClient.cpp):
//
//   LoadEntityState while !connected -> spdlog::error, callback({}) i.e. an
//                                       EMPTY state vector
//   SaveEntityState while !connected -> spdlog::error, callback(false)
//
// and adds a scriptable connected mode (StubEntityStoreControl::connected) so
// the load/save round trip and the empty-state branch in loadContainer can be
// observed. The control flag is used in place of the private connected_ member
// so that test code can flip it; a private member is not reachable from
// outside the class. It is a TEST DOUBLE, not a model of the wire protocol: it
// never opens a socket, never runs an io_context, never spawns a thread and
// never touches the network. The real client is exercised end-to-end by the
// headless gateway tests under test/integration/.

#include "stub_entity_state_client.h"

#include <apps/simcore/Network/clients/EntityStateStoreClient.h>

#include <cstdint>
#include <spdlog/spdlog.h>

namespace simcore {

StubEntityStoreControl g_stub;

EntityStateStoreClient::EntityStateStoreClient(asio::io_context& io)
    : io_(io), socket_(io), port_(0) {}

EntityStateStoreClient::~EntityStateStoreClient() = default;

void EntityStateStoreClient::LoadEntityState(
    int32_t dimension, int32_t x, int32_t y, int32_t z, uint16_t entity_type,
    LoadEntityStateCallback callback) {
  g_stub.load_calls.push_back({dimension, x, y, z, entity_type});
  if (!g_stub.connected) {
    // Faithful to EntityStateStoreClient.cpp: an unconnected client reports an
    // error and hands back an EMPTY state vector.
    spdlog::error("EntityStateStoreClient not connected");
    callback(EntityStateData{});
    return;
  }
  EntityStateData result;
  result.state = g_stub.load_state;
  callback(result);
}

void EntityStateStoreClient::SaveEntityState(
    int32_t dimension, int32_t x, int32_t y, int32_t z, uint16_t entity_type,
    const std::vector<uint8_t>& stateData, SaveEntityStateCallback callback) {
  g_stub.save_calls.push_back({dimension, x, y, z, entity_type, stateData});
  if (!g_stub.connected) {
    // Faithful to EntityStateStoreClient.cpp.
    spdlog::error("EntityStateStoreClient not connected");
    callback(false);
    return;
  }
  callback(g_stub.save_ok);
}

void EntityStateStoreClient::Connect(const std::string& host, uint16_t port) {
  host_ = host;
  port_ = port;
  doConnect(host, port);
}

void EntityStateStoreClient::Disconnect() {
  stopped_ = true;
  if (socket_.is_open()) socket_.close();
  connected_ = false;
  pending_callbacks_.clear();
  g_stub.connected = false;
}

void EntityStateStoreClient::doConnect(const std::string&, uint16_t) {
  // The stub never opens a socket; test code flips
  // StubEntityStoreControl::connected instead.
}

void EntityStateStoreClient::onConnect(const asio::error_code& ec) {
  connected_ = !ec;
  if (ec) {
    spdlog::error("EntityStateStoreClient connect error: {}", ec.message());
  }
}

void EntityStateStoreClient::writeFrame(const std::vector<uint8_t>&) {
  // No socket in the stub.
}

void EntityStateStoreClient::readFrame() {
  // No socket in the stub.
}

bool EntityStateStoreClient::IsConnected() const { return g_stub.connected; }

} // namespace simcore
