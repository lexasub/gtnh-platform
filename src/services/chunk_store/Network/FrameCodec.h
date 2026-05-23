#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

class FrameCodec {
public:
    static uint32_t readBE32(const uint8_t* buf) {
        return (static_cast<uint32_t>(buf[0]) << 24) |
               (static_cast<uint32_t>(buf[1]) << 16) |
               (static_cast<uint32_t>(buf[2]) <<  8) |
               (static_cast<uint32_t>(buf[3]));
    }

    static void writeBE32(uint8_t* buf, uint32_t v) {
        buf[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        buf[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        buf[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
        buf[3] = static_cast<uint8_t>( v        & 0xFF);
    }

    static void writeBE16(uint8_t* buf, uint16_t v) {
        buf[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
        buf[1] = static_cast<uint8_t>( v       & 0xFF);
    }

    static void buildSubscribeFrame(std::vector<uint8_t>& out, const std::string& topic) {
        uint32_t payload_len = 1 + 2 + topic.size();
        out.resize(4 + payload_len);
        writeBE32(out.data(), payload_len);
        out[4] = 0x01; // MsgSubscribe
        size_t off = 5;
        writeBE16(out.data() + off, static_cast<uint16_t>(topic.size()));
        off += 2;
        std::memcpy(out.data() + off, topic.data(), topic.size());
    }

    static void buildPublishFrame(std::vector<uint8_t>& out, const std::string& topic,
                                  const uint8_t* data, size_t data_len) {
        uint32_t payload_len = 1 + 2 + topic.size() + data_len;
        out.resize(4 + payload_len);
        writeBE32(out.data(), payload_len);
        out[4] = 0x03; // MsgPublish
        size_t off = 5;
        writeBE16(out.data() + off, static_cast<uint16_t>(topic.size()));
        off += 2;
        std::memcpy(out.data() + off, topic.data(), topic.size());
        off += topic.size();
        std::memcpy(out.data() + off, data, data_len);
    }

    static void buildRegisterFrame(std::vector<uint8_t>& out, const std::string& name,
                                   const std::vector<std::string>& topics) {
        size_t payload_len = 1 + 2 + name.size() + 2;
        for (const auto& t : topics) payload_len += 2 + t.size();
        out.resize(4 + payload_len);
        writeBE32(out.data(), payload_len);
        out[4] = 0x04; // MsgRegister
        size_t off = 5;
        writeBE16(out.data() + off, static_cast<uint16_t>(name.size()));
        off += 2;
        std::memcpy(out.data() + off, name.data(), name.size());
        off += name.size();
        writeBE16(out.data() + off, static_cast<uint16_t>(topics.size()));
        off += 2;
        for (const auto& t : topics) {
            writeBE16(out.data() + off, static_cast<uint16_t>(t.size()));
            off += 2;
            std::memcpy(out.data() + off, t.data(), t.size());
            off += t.size();
        }
    }
};