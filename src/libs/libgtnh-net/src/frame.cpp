#include <gtnh/net/frame.h>

#include <cstring>

namespace gtnh::net::frame {

void write_be32(uint8_t* buf, uint32_t v) {
    buf[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    buf[3] = static_cast<uint8_t>( v        & 0xFF);
}

void write_be16(uint8_t* buf, uint16_t v) {
    buf[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[1] = static_cast<uint8_t>( v       & 0xFF);
}

uint32_t read_be32(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) |
           (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) <<  8) |
           (static_cast<uint32_t>(buf[3]));
}

std::shared_ptr<std::vector<uint8_t>> pack(uint8_t type,
                                            const uint8_t* data,
                                            size_t len) {
    uint32_t total_len = 1 + static_cast<uint32_t>(len);
    auto frame = std::make_shared<std::vector<uint8_t>>(4 + total_len);
    write_be32(frame->data(), total_len);
    (*frame)[4] = type;
    if (len > 0 && data) {
        std::memcpy(frame->data() + 5, data, len);
    }
    return frame;
}

std::shared_ptr<std::vector<uint8_t>> pack_router(uint8_t router_msg_type,
                                                    const uint8_t* payload,
                                                    size_t payload_len) {
    // Router frame: [4B BE len][1B type][payload]
    // len includes the type byte + payload
    uint32_t total_len = 1 + static_cast<uint32_t>(payload_len);
    auto frame = std::make_shared<std::vector<uint8_t>>(4 + total_len);
    write_be32(frame->data(), total_len);
    (*frame)[4] = router_msg_type;
    if (payload_len > 0 && payload) {
        std::memcpy(frame->data() + 5, payload, payload_len);
    }
    return frame;
}

} // namespace gtnh::net::frame
