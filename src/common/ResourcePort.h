#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace gtnh::common {

using PortId = std::uint64_t;

enum class ResourceKind : std::uint8_t {
  FLUID = 0,
  EU = 1,
  HU = 2,
  RU = 3,
  ITEM = 4,
};

enum class PortRole : std::uint8_t {
  NONE = 0,
  SOURCE = 1,
  SINK = 2,
};

enum class FacePolicy : std::uint8_t {
  ALL_FACES = 0,
  MASK = 1,
};

struct ResourcePortRegistrationKey {
  std::uint64_t owner_id = 0;
  ResourceKind resource_kind = ResourceKind::FLUID;
  PortId port_id = 0;
  std::uint64_t epoch = 0;
  friend bool operator==(const ResourcePortRegistrationKey&, const ResourcePortRegistrationKey&) = default;
};

struct ResourcePortRegistrationKeyHash {
  [[nodiscard]] std::size_t operator()(const ResourcePortRegistrationKey& key) const noexcept {
    std::size_t h = std::hash<std::uint64_t>{}(key.owner_id);
    h ^= std::hash<std::uint64_t>{}(key.port_id) + (h << 6) + (h >> 2);
    h ^= std::hash<std::uint64_t>{}(key.epoch) + (h << 6) + (h >> 2);
    h ^= std::hash<unsigned>{}(static_cast<unsigned>(key.resource_kind)) + (h << 6) + (h >> 2);
    return h;
  }
};

// One transport-facing machine port. A port carries no resource filter: it
// says only which channel and direction it supports; concrete FLUID/ITEM
// operation messages carry the actual resource ID.
struct ResourcePort {
  PortId port_id = 0;
  std::uint64_t owner_id = 0;
  ResourceKind resource_kind = ResourceKind::FLUID;
  PortRole role = PortRole::NONE;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t z = 0;
  FacePolicy face_policy = FacePolicy::ALL_FACES;
  std::uint8_t face_mask = 0x3f;
  std::int32_t capacity = 0;
  std::int32_t rate = 0;
  std::uint64_t epoch = 0;

  [[nodiscard]] bool valid() const { return port_id != 0; }
  [[nodiscard]] ResourcePortRegistrationKey registrationKey() const {
    return {owner_id, resource_kind, port_id, epoch};
  }
};

} // namespace gtnh::common

namespace std {
template<> struct hash<gtnh::common::ResourcePortRegistrationKey>
    : gtnh::common::ResourcePortRegistrationKeyHash {};
} // namespace std
