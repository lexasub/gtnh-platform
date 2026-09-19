#include "SectionCodec.h"

uint32_t BitReader::read(int bits) {
    uint32_t val = 0;
    for (int i = 0; i < bits; ++i) {
        if ((data[bit_pos / 8] >> (bit_pos % 8)) & 1)
            val |= (1u << i);
        ++bit_pos;
    }
    return val;
}

void writeU8(std::vector<uint8_t> &buf, uint8_t v) { buf.push_back(v); }

void writeU16(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void writeU32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

bool Reader::readU8(uint8_t &v) {
    if (pos + 1 > size)
        return false;
    v = data[pos++];
    return true;
}

bool Reader::readU16(uint16_t &v) {
    if (pos + 2 > size)
        return false;
    v = static_cast<uint16_t>(data[pos]) |
        (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return true;
}

bool Reader::readU32(uint32_t &v) {
    if (pos + 4 > size)
        return false;
    v = static_cast<uint32_t>(data[pos]) |
        (static_cast<uint32_t>(data[pos + 1]) << 8) |
        (static_cast<uint32_t>(data[pos + 2]) << 16) |
        (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

bool Reader::skip(size_t n) {
    if (pos + n > size)
        return false;
    pos += n;
    return true;
}
