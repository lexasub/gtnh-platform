#pragma once

// Shared client API for the typed resource-port contract (openspec
// refactor-fluid-port-accounting, items 2.2/3.1). Boiler publishers, owner
// drain handlers, and the pipe service implement against this one interface
// instead of hand-rolling FlatBuffers builders per service.
//
// Header-only. Requires the generated core.fbs + pipe_network.fbs headers
// (<core_generated.h> / <pipe_network_generated.h>) on the include path;
// every service participating in the typed port contract already generates
// them.

#include <common/ResourcePort.h>

#include <core_generated.h>
#include <pipe_network_generated.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace gtnh::common {

// Canonical typed port topics (MessageRouter pub/sub).
inline constexpr const char* kTopicResourcePortRegister = "resource.port.register";
inline constexpr const char* kTopicResourcePortRemove = "resource.port.remove";
inline constexpr const char* kTopicResourceDrainRequest = "resource.drain.request";
inline constexpr const char* kTopicResourceDrainResponse = "resource.drain.response";
inline constexpr const char* kTopicResourceConsumeRequest = "resource.consume.request";
inline constexpr const char* kTopicResourceConsumeResponse = "resource.consume.response";

// One drain/consume transaction. resource_id is the packed canonical ItemId
// for FLUID/ITEM and zero for energy channels.
struct ResourceTransferRequest {
  std::uint64_t request_id = 0;
  PortId port_id = 0;
  ResourceKind resource_kind = ResourceKind::FLUID;
  std::uint32_t resource_id = 0;
  std::int32_t amount = 0;
};

// accepted_amount <= amount; remaining is only meaningful for consume
// responses (drain responses leave it 0).
struct ResourceTransferResponse {
  std::uint64_t request_id = 0;
  PortId port_id = 0;
  ResourceKind resource_kind = ResourceKind::FLUID;
  std::uint32_t resource_id = 0;
  std::int32_t accepted_amount = 0;
  std::int32_t remaining = 0;
};

// Domain <-> wire enum mapping. Both enums share values by design; a
// malformed wire byte yields an out-of-range domain value that handlers must
// reject semantically.
[[nodiscard]] inline Protocol::ResourceKind ToWire(ResourceKind kind) {
  return static_cast<Protocol::ResourceKind>(static_cast<std::uint8_t>(kind));
}
[[nodiscard]] inline ResourceKind FromWire(Protocol::ResourceKind kind) {
  return static_cast<ResourceKind>(static_cast<std::uint8_t>(kind));
}
[[nodiscard]] inline Protocol::PortRole ToWire(PortRole role) {
  return static_cast<Protocol::PortRole>(static_cast<std::uint8_t>(role));
}
[[nodiscard]] inline PortRole FromWire(Protocol::PortRole role) {
  return static_cast<PortRole>(static_cast<std::uint8_t>(role));
}
[[nodiscard]] inline Protocol::FacePolicy ToWire(FacePolicy policy) {
  return static_cast<Protocol::FacePolicy>(static_cast<std::uint8_t>(policy));
}
[[nodiscard]] inline FacePolicy FromWire(Protocol::FacePolicy policy) {
  return static_cast<FacePolicy>(static_cast<std::uint8_t>(policy));
}

// --- Serialization: domain structs -> FlatBuffers payloads for the topics
// above. The register payload carries resource_id separately from the port
// record: the port itself has no resource filter, the message advertises the
// current resource snapshot (packed ItemId; 0 for energy channels). ---

[[nodiscard]] inline std::vector<std::uint8_t> SerializePortRegister(
    const ResourcePort& port, std::uint32_t resource_id) {
  flatbuffers::FlatBufferBuilder fbb;
  const Protocol::Vec3i pos(port.x, port.y, port.z);
  Protocol::ResourcePortRegisterBuilder builder(fbb);
  builder.add_port_id(port.port_id);
  builder.add_owner_id(port.owner_id);
  builder.add_resource_kind(ToWire(port.resource_kind));
  builder.add_resource_id(resource_id);
  builder.add_role(ToWire(port.role));
  builder.add_pos(&pos);
  builder.add_capacity(port.capacity);
  builder.add_rate(port.rate);
  builder.add_epoch(port.epoch);
  builder.add_face_policy(ToWire(port.face_policy));
  builder.add_face_mask(port.face_mask);
  fbb.Finish(builder.Finish());
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

[[nodiscard]] inline std::vector<std::uint8_t> SerializePortRemove(
    std::uint64_t owner_id, ResourceKind kind, PortId port_id,
    std::uint64_t epoch) {
  flatbuffers::FlatBufferBuilder fbb;
  Protocol::ResourcePortRemoveBuilder builder(fbb);
  builder.add_port_id(port_id);
  builder.add_owner_id(owner_id);
  builder.add_epoch(epoch);
  builder.add_resource_kind(ToWire(kind));
  fbb.Finish(builder.Finish());
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

[[nodiscard]] inline std::vector<std::uint8_t> SerializeDrainRequest(
    const ResourceTransferRequest& request) {
  flatbuffers::FlatBufferBuilder fbb;
  Protocol::ResourceDrainRequestBuilder builder(fbb);
  builder.add_request_id(request.request_id);
  builder.add_port_id(request.port_id);
  builder.add_resource_kind(ToWire(request.resource_kind));
  builder.add_resource_id(request.resource_id);
  builder.add_amount(request.amount);
  fbb.Finish(builder.Finish());
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

[[nodiscard]] inline std::vector<std::uint8_t> SerializeConsumeRequest(
    const ResourceTransferRequest& request) {
  flatbuffers::FlatBufferBuilder fbb;
  Protocol::ResourceConsumeRequestBuilder builder(fbb);
  builder.add_request_id(request.request_id);
  builder.add_port_id(request.port_id);
  builder.add_resource_kind(ToWire(request.resource_kind));
  builder.add_resource_id(request.resource_id);
  builder.add_amount(request.amount);
  fbb.Finish(builder.Finish());
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

// --- Parsing: wire payloads -> domain structs. Returns false on verifier
// failure; callers still validate semantics (epoch, amounts, port state). ---

[[nodiscard]] inline bool ParsePortRegister(const std::uint8_t* data,
                                            std::size_t size,
                                            ResourcePort* out_port,
                                            std::uint32_t* out_resource_id) {
  flatbuffers::Verifier verifier(data, size);
  const auto* msg = flatbuffers::GetRoot<Protocol::ResourcePortRegister>(data);
  if (msg == nullptr || !msg->Verify(verifier)) return false;
  const auto* pos = msg->pos();
  *out_port = ResourcePort{
      .port_id = msg->port_id(),
      .owner_id = msg->owner_id(),
      .resource_kind = FromWire(msg->resource_kind()),
      .role = FromWire(msg->role()),
      .x = pos->x(),
      .y = pos->y(),
      .z = pos->z(),
      .face_policy = FromWire(msg->face_policy()),
      .face_mask = msg->face_mask(),
      .capacity = msg->capacity(),
      .rate = msg->rate(),
      .epoch = msg->epoch(),
  };
  *out_resource_id = msg->resource_id();
  return true;
}

[[nodiscard]] inline bool ParsePortRemove(const std::uint8_t* data,
                                          std::size_t size,
                                          ResourcePortRegistrationKey* out_key) {
  flatbuffers::Verifier verifier(data, size);
  const auto* msg = flatbuffers::GetRoot<Protocol::ResourcePortRemove>(data);
  if (msg == nullptr || !msg->Verify(verifier)) return false;
  *out_key = ResourcePortRegistrationKey{
      .owner_id = msg->owner_id(),
      .resource_kind = FromWire(msg->resource_kind()),
      .port_id = msg->port_id(),
      .epoch = msg->epoch(),
  };
  return true;
}

[[nodiscard]] inline bool ParseDrainRequest(const std::uint8_t* data,
                                            std::size_t size,
                                            ResourceTransferRequest* out) {
  flatbuffers::Verifier verifier(data, size);
  const auto* msg = flatbuffers::GetRoot<Protocol::ResourceDrainRequest>(data);
  if (msg == nullptr || !msg->Verify(verifier)) return false;
  *out = ResourceTransferRequest{
      .request_id = msg->request_id(),
      .port_id = msg->port_id(),
      .resource_kind = FromWire(msg->resource_kind()),
      .resource_id = msg->resource_id(),
      .amount = msg->amount(),
  };
  return true;
}

[[nodiscard]] inline bool ParseConsumeRequest(const std::uint8_t* data,
                                              std::size_t size,
                                              ResourceTransferRequest* out) {
  flatbuffers::Verifier verifier(data, size);
  const auto* msg = flatbuffers::GetRoot<Protocol::ResourceConsumeRequest>(data);
  if (msg == nullptr || !msg->Verify(verifier)) return false;
  *out = ResourceTransferRequest{
      .request_id = msg->request_id(),
      .port_id = msg->port_id(),
      .resource_kind = FromWire(msg->resource_kind()),
      .resource_id = msg->resource_id(),
      .amount = msg->amount(),
  };
  return true;
}

[[nodiscard]] inline bool ParseDrainResponse(const std::uint8_t* data,
                                             std::size_t size,
                                             ResourceTransferResponse* out) {
  flatbuffers::Verifier verifier(data, size);
  const auto* msg = flatbuffers::GetRoot<Protocol::ResourceDrainResponse>(data);
  if (msg == nullptr || !msg->Verify(verifier)) return false;
  *out = ResourceTransferResponse{
      .request_id = msg->request_id(),
      .port_id = msg->port_id(),
      .resource_kind = FromWire(msg->resource_kind()),
      .resource_id = msg->resource_id(),
      .accepted_amount = msg->accepted_amount(),
      .remaining = 0,
  };
  return true;
}

[[nodiscard]] inline bool ParseConsumeResponse(const std::uint8_t* data,
                                               std::size_t size,
                                               ResourceTransferResponse* out) {
  flatbuffers::Verifier verifier(data, size);
  const auto* msg = flatbuffers::GetRoot<Protocol::ResourceConsumeResponse>(data);
  if (msg == nullptr || !msg->Verify(verifier)) return false;
  *out = ResourceTransferResponse{
      .request_id = msg->request_id(),
      .port_id = msg->port_id(),
      .resource_kind = FromWire(msg->resource_kind()),
      .resource_id = msg->resource_id(),
      .accepted_amount = msg->accepted_amount(),
      .remaining = msg->remaining(),
  };
  return true;
}

// Transport-agnostic typed sender. Implement over any pub/sub transport, or
// bind a generic (topic, payload) publisher with TypedResourcePortClient.
class IResourcePortClient {
 public:
  virtual ~IResourcePortClient() = default;

  virtual bool PublishPortRegister(const ResourcePort& port,
                                   std::uint32_t resource_id) = 0;
  virtual bool PublishPortRemove(std::uint64_t owner_id, ResourceKind kind,
                                 PortId port_id, std::uint64_t epoch) = 0;
  virtual bool PublishDrainRequest(const ResourceTransferRequest& request) = 0;
  virtual bool PublishConsumeRequest(const ResourceTransferRequest& request) = 0;
};

// Adapter binding the typed interface to a generic topic publisher (e.g. a
// MessageRouter client's Publish(topic, bytes)).
class TypedResourcePortClient final : public IResourcePortClient {
 public:
  using Publisher = std::function<bool(
      const char* topic, const std::vector<std::uint8_t>& payload)>;

  explicit TypedResourcePortClient(Publisher publisher)
      : publisher_(std::move(publisher)) {}

  bool PublishPortRegister(const ResourcePort& port,
                           std::uint32_t resource_id) override {
    return publisher_(kTopicResourcePortRegister,
                      SerializePortRegister(port, resource_id));
  }
  bool PublishPortRemove(std::uint64_t owner_id, ResourceKind kind,
                         PortId port_id, std::uint64_t epoch) override {
    return publisher_(kTopicResourcePortRemove,
                      SerializePortRemove(owner_id, kind, port_id, epoch));
  }
  bool PublishDrainRequest(const ResourceTransferRequest& request) override {
    return publisher_(kTopicResourceDrainRequest,
                      SerializeDrainRequest(request));
  }
  bool PublishConsumeRequest(const ResourceTransferRequest& request) override {
    return publisher_(kTopicResourceConsumeRequest,
                      SerializeConsumeRequest(request));
  }

 private:
  Publisher publisher_;
};

}  // namespace gtnh::common
