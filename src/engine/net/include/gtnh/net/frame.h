#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace gtnh::net::frame {

// Wire header size: [4 bytes: payload length BE][1 byte: message type]
inline constexpr size_t kHeaderSize = 5;

// Write big-endian 32-bit value into buf
void write_be32(uint8_t *buf, uint32_t v);

// Write big-endian 16-bit value into buf
void write_be16(uint8_t *buf, uint16_t v);

// Read big-endian 32-bit value from 4 bytes
uint32_t read_be32(const uint8_t *buf);

// Build a complete wire frame: [4B BE len][1B type][payload]
// len includes the type byte + payload size.
// Returns shared_ptr for zero-copy send_raw.
std::shared_ptr<std::vector<uint8_t>> pack(uint8_t type, const uint8_t *data,
                                           size_t len);

// Build a router protocol frame:
// [4B BE len][1B router_msg_type][router_payload]
std::shared_ptr<std::vector<uint8_t>> pack_router(uint8_t router_msg_type,
                                                  const uint8_t *payload,
                                                  size_t payload_len);

} // namespace gtnh::net::frame
