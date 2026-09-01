#pragma once

// ---------------------------------------------------------------------------
// Codec for the client-facing ResourceBufferState message (openspec
// refactor-fluid-port-accounting §5). Header-only, mirrors the
// ResourcePortClient.h pattern: services that participate in the client-state
// contract (simulation_core publisher, gateway verifier, game client store)
// generate <client_state_generated.h> and share this one serialization path,
// so the fixture test exercises the exact wire bytes both sides produce.
// ---------------------------------------------------------------------------

#include <client_state_generated.h>

#include <common/ResourcePort.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gtnh::common {

// Router topic for client-facing machine/port buffer state. The gateway
// subscribes to exactly this topic; internal PipeNetwork topics
// (resource.port.register/remove, resource.drain.*, resource.consume.*,
// fluid.node.update, fluid.flow) are never forwarded to clients.
inline constexpr const char* kTopicResourceBufferState =
    "sim.resource.buffer.state";

struct ResourceBufferStateMsg {
  std::uint64_t owner_id = 0;
  std::uint64_t port_id = 0;
  ResourceKind resource_kind = ResourceKind::FLUID;
  std::uint32_t resource_id = 0;
  std::int32_t amount = 0;
  std::int32_t capacity = 0;
  std::int32_t rate = 0;
  std::uint64_t epoch = 0;
  std::uint64_t sequence = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t z = 0;
  bool removed = false;
};

[[nodiscard]] inline std::vector<std::uint8_t> SerializeResourceBufferState(
    const ResourceBufferStateMsg& state) {
  flatbuffers::FlatBufferBuilder fbb;
  const Protocol::Vec3i pos(state.x, state.y, state.z);
  Protocol::ResourceBufferStateBuilder builder(fbb);
  builder.add_owner_id(state.owner_id);
  builder.add_port_id(state.port_id);
  builder.add_resource_kind(
      static_cast<Protocol::ResourceKind>(static_cast<std::uint8_t>(state.resource_kind)));
  builder.add_resource_id(state.resource_id);
  builder.add_amount(state.amount);
  builder.add_capacity(state.capacity);
  builder.add_rate(state.rate);
  builder.add_epoch(state.epoch);
  builder.add_sequence(state.sequence);
  builder.add_pos(&pos);
  builder.add_removed(state.removed);
  // The generated table-builder Finish() only ends the table; the buffer
  // itself is finalized by FlatBufferBuilder::Finish(offset).
  fbb.Finish(builder.Finish());
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

// Verifying parse. Returns false on any malformed buffer; callers must treat
// a false return as "drop the update" (fail closed).
[[nodiscard]] inline bool ParseResourceBufferState(
    const std::uint8_t* data, std::size_t size, ResourceBufferStateMsg* out) {
  if (data == nullptr || out == nullptr) return false;
  flatbuffers::Verifier verifier(data, size);
  const auto* msg = flatbuffers::GetRoot<Protocol::ResourceBufferState>(data);
  if (msg == nullptr || !msg->Verify(verifier)) return false;
  const auto* pos = msg->pos();
  if (pos == nullptr) return false;
  const auto wire_kind = static_cast<std::uint8_t>(msg->resource_kind());
  if (wire_kind > static_cast<std::uint8_t>(ResourceKind::ITEM)) return false;
  if (msg->amount() < 0 || msg->capacity() < 0 || msg->rate() < 0) return false;
  *out = ResourceBufferStateMsg{
      .owner_id = msg->owner_id(),
      .port_id = msg->port_id(),
      .resource_kind = static_cast<ResourceKind>(wire_kind),
      .resource_id = msg->resource_id(),
      .amount = msg->amount(),
      .capacity = msg->capacity(),
      .rate = msg->rate(),
      .epoch = msg->epoch(),
      .sequence = msg->sequence(),
      .x = pos->x(),
      .y = pos->y(),
      .z = pos->z(),
      .removed = msg->removed(),
  };
  return true;
}

}  // namespace gtnh::common
