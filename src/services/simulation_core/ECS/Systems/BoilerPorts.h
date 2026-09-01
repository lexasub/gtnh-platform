#pragma once

// Typed resource-port allocation for the steam heat boiler (openspec
// refactor-fluid-port-accounting, item 2.4). Header-only and FlatBuffers-free:
// wire serialization lives in common/ResourcePortClient.h.
//
// Port-id scheme (2.4.1) — deterministic and stable across ticks and restarts
// for the same machine instance:
//
//   port_id = (machine_class << 8) | slot
//
//   machine_class : stable small integer per machine family (heat boiler = 1)
//   slot          : stable per-role index inside the family
//                   (1 = HU sink, 2 = FLUID steam source)
//
// owner_id (the EnTT entity id; 0 is a valid owner) disambiguates instances of
// the same family, so port_id never depends on entity ids, ticks, or session
// state. Registration is idempotent by (owner, resource kind, port, epoch);
// epochs are monotonic per port and owned by the publisher
// (replacement = epoch + 1).

#include <common/ResourcePort.h>

#include <cstdint>
#include <map>
#include <utility>

namespace simcore::BoilerPorts {

inline constexpr std::uint64_t kMachineClassHeatBoiler = 1;
inline constexpr std::uint64_t kSlotHuSink = 1;
inline constexpr std::uint64_t kSlotSteamSource = 2;

[[nodiscard]] constexpr gtnh::common::PortId MakeMachinePortId(
    std::uint64_t machine_class, std::uint64_t slot) {
  return (machine_class << 8) | slot;
}

// 0x0101 / 0x0102 — distinct by construction.
inline constexpr gtnh::common::PortId kBoilerHuSinkPortId =
    MakeMachinePortId(kMachineClassHeatBoiler, kSlotHuSink);
inline constexpr gtnh::common::PortId kBoilerSteamSourcePortId =
    MakeMachinePortId(kMachineClassHeatBoiler, kSlotSteamSource);

// Pure port factories: each record is a function of its arguments only, so
// publishing HU-before-FLUID or FLUID-before-HU (or republishing every tick)
// produces identical, mutually independent records (order independence, 2.4.3).

[[nodiscard]] inline gtnh::common::ResourcePort MakeHuSinkPort(
    std::uint64_t owner_id, std::int32_t x, std::int32_t y, std::int32_t z,
    std::int32_t hu_capacity, std::int32_t hu_rate, std::uint64_t epoch) {
  gtnh::common::ResourcePort port;
  port.port_id = kBoilerHuSinkPortId;
  port.owner_id = owner_id;
  port.resource_kind = gtnh::common::ResourceKind::HU;
  port.role = gtnh::common::PortRole::SINK;
  port.x = x;
  port.y = y;
  port.z = z;
  port.face_policy = gtnh::common::FacePolicy::ALL_FACES;
  port.face_mask = 0x3f;  // all six faces (legacy boiler has no face restriction)
  port.capacity = hu_capacity;
  port.rate = hu_rate;
  port.epoch = epoch;
  return port;
}

[[nodiscard]] inline gtnh::common::ResourcePort MakeSteamSourcePort(
    std::uint64_t owner_id, std::int32_t x, std::int32_t y, std::int32_t z,
    std::int32_t fluid_capacity, std::int32_t fluid_rate, std::uint64_t epoch) {
  gtnh::common::ResourcePort port;
  port.port_id = kBoilerSteamSourcePortId;
  port.owner_id = owner_id;
  port.resource_kind = gtnh::common::ResourceKind::FLUID;
  port.role = gtnh::common::PortRole::SOURCE;
  port.x = x;
  port.y = y;
  port.z = z;
  port.face_policy = gtnh::common::FacePolicy::ALL_FACES;
  port.face_mask = 0x3f;
  port.capacity = fluid_capacity;
  port.rate = fluid_rate;
  port.epoch = epoch;
  return port;
}

// Monotonic per-port epoch counters owned by the publisher. First sight of a
// port allocates epoch 1; Replace() bumps that port only — other ports of the
// same owner keep their epoch.
class PortEpochBook {
 public:
  [[nodiscard]] std::uint64_t EpochOf(std::uint64_t owner_id,
                                      gtnh::common::PortId port_id) {
    const auto it = epochs_.find({owner_id, port_id});
    return it != epochs_.end() ? it->second : kFirstEpoch;
  }

  // Announce a replacement of one port: epoch + 1, monotonic per port.
  std::uint64_t Replace(std::uint64_t owner_id, gtnh::common::PortId port_id) {
    std::uint64_t& epoch = epochs_[{owner_id, port_id}];
    if (epoch < kFirstEpoch) epoch = kFirstEpoch;
    return ++epoch;
  }

 private:
  static constexpr std::uint64_t kFirstEpoch = 1;
  std::map<std::pair<std::uint64_t, gtnh::common::PortId>, std::uint64_t>
      epochs_;
};

}  // namespace simcore::BoilerPorts
