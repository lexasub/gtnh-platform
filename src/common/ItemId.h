#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// ItemId: hierarchical prefix-based item ID encoding.
//
// Format in CSV:     "prefix:subprefix:...:payload"
//   - All segments before the LAST colon are binary prefix bits (0/1 chars)
//   - The LAST segment is a decimal payload number
//
// Packing: concatenate all prefix bits, shift left by (16 - prefix_len),
//          OR with the payload.
//
// Decode: top-down comparison — find the top-level prefix (0, 10, 110, etc.),
//         then extract the remaining bits.
//
// Category layout (prefix-free by construction):
//   0               BASE blocks/items                     (15-bit payload)
//   10              ORES                                  (14-bit payload)
//   110             MATERIALS (ingots, plates, dusts)     (13-bit payload)
//   1110            MACHINES                              (12-bit payload)
//   1111            TOOLS + INFRA (sub-prefixed inside)   (12-bit payload)
//
// Example: "0:10:3"  → prefix bits "010", payload 3 → 0x4003
//          "1111:0:5" → prefix bits "11110", payload 5 → 0xF005
// ---------------------------------------------------------------------------
namespace ItemId {

// ---------------------------------------------------------------------------
// Category enum
// ---------------------------------------------------------------------------
enum Category : uint8_t {
  CAT_BASE = 0,
  CAT_ORES = 1,
  CAT_MATERIALS = 2,
  CAT_MACHINES = 3,
  CAT_INFRA = 4, // tools, cables, pipes, fluid items
};

/// Decode top-level category from a packed ID based on prefix bits.
constexpr Category category(uint16_t id) {
  if (id < 0x8000)
    return CAT_BASE;
  if (id < 0xC000)
    return CAT_ORES;
  if (id < 0xE000)
    return CAT_MATERIALS;
  if (id < 0xF000)
    return CAT_MACHINES;
  return CAT_INFRA;
}

/// Human-readable category name.
constexpr const char *categoryName(Category c) {
  switch (c) {
  case CAT_BASE:
    return "BASE";
  case CAT_ORES:
    return "ORES";
  case CAT_MATERIALS:
    return "MATERIALS";
  case CAT_MACHINES:
    return "MACHINES";
  case CAT_INFRA:
    return "INFRA";
  default:
    return "?";
  }
}

constexpr const char *categoryName(uint16_t id) {
  return categoryName(category(id));
}

// ---------------------------------------------------------------------------
// Pack: "1110:10:5" → uint16_t
//
// Split on ':', concatenate binary segments, last segment is decimal payload.
// ---------------------------------------------------------------------------
constexpr uint16_t pack(std::string_view s) {
  if (s.empty())
    return 0;

  // Find last colon
  auto last_colon = s.rfind(':');
  if (last_colon == std::string_view::npos) {
    // No colons → plain decimal (backward compat with flat IDs)
    uint16_t val = 0;
    for (char c : s) {
      if (c >= '0' && c <= '9')
        val = static_cast<uint16_t>(val * 10 + (c - '0'));
    }
    return val;
  }

  // Parse prefix bits (binary 0/1 before last colon, skipping colons)
  uint16_t prefix = 0;
  int plen = 0;
  for (size_t i = 0; i < last_colon; ++i) {
    char c = s[i];
    if (c == '0') {
      prefix = static_cast<uint16_t>((prefix << 1) | 0);
      ++plen;
    } else if (c == '1') {
      prefix = static_cast<uint16_t>((prefix << 1) | 1);
      ++plen;
    }
    // skip ':' (separator)
  }

  if (plen > 15)
    return 0; // too many prefix bits

  // Parse payload (decimal after last colon)
  uint16_t payload = 0;
  for (size_t i = last_colon + 1; i < s.size(); ++i) {
    char c = s[i];
    if (c >= '0' && c <= '9')
      payload = static_cast<uint16_t>(payload * 10 + (c - '0'));
  }

  // Shift: prefix occupies top 'plen' bits, payload fills the rest
  int shift = 16 - plen;
  return static_cast<uint16_t>((static_cast<unsigned>(prefix) << shift) |
                               payload);
}

// ---------------------------------------------------------------------------
// Unpack: uint16_t → human-readable "prefix:payload" (top level only)
// Sub-prefix levels not decoded — use category-specific helpers for that.
// ---------------------------------------------------------------------------
inline std::string unpack(uint16_t id) {
  if (id < 0x8000) {
    return "0:" + std::to_string(id & 0x7FFF);
  }
  if (id < 0xC000) {
    return "10:" + std::to_string(id & 0x3FFF);
  }
  if (id < 0xE000) {
    return "110:" + std::to_string(id & 0x1FFF);
  }
  if (id < 0xF000) {
    return "1110:" + std::to_string(id & 0xFFF);
  }
  return "1111:" + std::to_string(id & 0xFFF);
}

// ---------------------------------------------------------------------------
// Convenience: tool tier from tool item ID (items under 1111:00: prefix)
//   Tool items are packed with prefix "111100" (6 bits),
//   payload = [tier:5][type:5]
//   tier = (payload >> 5) & 0x1F
//   type = payload & 0x1F
// ---------------------------------------------------------------------------
constexpr int toolTier(uint16_t id) {
  // Check top 6 bits = 111100 (0xF000–0xF3FF range)
  if ((id & 0xFC00) != 0xF000) {
    return 0; // not a tool
  }
  // Payload is lower 10 bits; tier is high 5 bits of payload
  uint16_t payload = id & 0x03FF;
  return (payload >> 5) & 0x1F;
}

constexpr int toolType(uint16_t id) {
  if ((id & 0xFC00) != 0xF000)
    return 0;
  uint16_t payload = id & 0x03FF;
  return payload & 0x1F;
}

// NOLINTBEGIN
// Category-specific helpers can go here:
//   isOre(id), materialFamily(id), machineTier(id), etc.
// NOLINTEND

} // namespace ItemId
