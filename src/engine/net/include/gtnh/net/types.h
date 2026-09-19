#pragma once

#include <cstddef>
#include <cstdint>

namespace gtnh::net {

// Maximum allowed payload per frame (4 MiB)
inline constexpr size_t kMaxPayload = 4 * 1024 * 1024;

// Default io_uring queue depth per ring
inline constexpr unsigned kDefaultRingEntries = 256;

// Bits reserved for tag in user_data (lower kTagBits bits)
// Upper bits encode generation for stale CQE rejection.
inline constexpr uint64_t kTagBits = 24;
inline constexpr uint64_t kTagMask = (uint64_t(1) << kTagBits) - 1;

// Encode user_data from generation and raw tag:
//   user_data = (generation << kTagBits) | (raw_tag & kTagMask)
inline constexpr uint64_t encode_user_data(uint64_t generation,
                                           uint64_t raw_tag) {
  return (generation << kTagBits) | (raw_tag & kTagMask);
}

// Decode raw tag from user_data
inline constexpr uint64_t decode_tag(uint64_t user_data) {
  return user_data & kTagMask;
}

// Decode generation from user_data
inline constexpr uint64_t decode_generation(uint64_t user_data) {
  return user_data >> kTagBits;
}

} // namespace gtnh::net
