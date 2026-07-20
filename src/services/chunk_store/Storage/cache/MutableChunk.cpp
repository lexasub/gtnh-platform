#include "MutableChunk.h"

uint16_t MutableChunk::getBlock(int x, int y, int z) const {
    int sec = ((y >> 4) << 2) | ((z >> 4) << 1) | (x >> 4);
    int li = ((y & 0xF) << 8) | ((z & 0xF) << 4) | (x & 0xF);
    return sections[sec].getBlock(li);
}

void MutableChunk::setBlock(int x, int y, int z, uint16_t id) {
    int sec = ((y >> 4) << 2) | ((z >> 4) << 1) | (x >> 4);
    int li = ((y & 0xF) << 8) | ((z & 0xF) << 4) | (x & 0xF);
    sections[sec].setBlock(li, id);
}

uint8_t MutableChunk::getMeta(int x, int y, int z) const {
    int sec = ((y >> 4) << 2) | ((z >> 4) << 1) | (x >> 4);
    int li = ((y & 0xF) << 8) | ((z & 0xF) << 4) | (x & 0xF);
    return sections[sec].getMeta(li);
}

void MutableChunk::setMeta(int x, int y, int z, uint8_t m) {
    int sec = ((y >> 4) << 2) | ((z >> 4) << 1) | (x >> 4);
    int li = ((y & 0xF) << 8) | ((z & 0xF) << 4) | (x & 0xF);
    sections[sec].setMeta(li, m);
}

uint32_t MutableChunk::getMultiblock(int x, int y, int z) const {
    int sec = ((y >> 4) << 2) | ((z >> 4) << 1) | (x >> 4);
    int li = ((y & 0xF) << 8) | ((z & 0xF) << 4) | (x & 0xF);
    return sections[sec].getMultiblock(li);
}

void MutableChunk::setMultiblock(int x, int y, int z, uint32_t mb_id) {
    int sec = ((y >> 4) << 2) | ((z >> 4) << 1) | (x >> 4);
    int li = ((y & 0xF) << 8) | ((z & 0xF) << 4) | (x & 0xF);
    sections[sec].setMultiblock(li, mb_id);
}

MutableChunk MutableChunk::fromBlocks(const uint16_t blocks[SEC_VOL * SEC_CNT]) {
    MutableChunk mc;  // TODO restrict, use ptrs
    for (int s = 0; s < SEC_CNT; ++s) {
        int ox = sectionOrigin(s, 0);
        int oy = sectionOrigin(s, 2);
        int oz = sectionOrigin(s, 1);

        alignas(64) uint16_t local[SEC_VOL];
        for (int ly = 0; ly < SEC_SZ; ++ly) {
            for (int lz = 0; lz < SEC_SZ; ++lz) {
                for (int lx = 0; lx < SEC_SZ; ++lx) {
                    local[localIndex(lx, ly, lz)] =
                            blocks[chunkIndex(ox + lx, oy + ly, oz + lz)];
                }
            }
        }
        mc.sections[s].fromBlocks(local);
    }
    return mc;
}

void MutableChunk::encodeToWire(std::vector<uint8_t> &buf) const {
    buf.clear();
    writeU32(buf, MAGIC);
    writeU8(buf, 1); // version
    writeU8(buf, SEC_CNT);
    for (int s = 0; s < SEC_CNT; ++s)
        sections[s].encodeToWire(buf);
}

bool MutableChunk::fromWire(const uint8_t *data, size_t size) {
    Reader r{data, size, 0};

    uint32_t magic = 0;
    if (!r.readU32(magic) || magic != MAGIC) return false;
    uint8_t ver = 0;
    if (!r.readU8(ver) || ver < 1) [[unlikely]] return false;
    uint8_t sec_cnt = 0;
    if (!r.readU8(sec_cnt) || sec_cnt != SEC_CNT) return false;

    for (int s = 0; s < SEC_CNT; ++s) {
        if (!sections[s].fromWire(r)) [[unlikely]] return false;
    }
    return true;
}
