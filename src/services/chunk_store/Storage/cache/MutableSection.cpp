#include "MutableSection.h"
#include "Metrics.h"
#include <chrono>
#include <cstring>

MutableSection::FlatData::FlatData(FlatData&& o) noexcept
    : palette(std::move(o.palette)), indices(o.indices),
      indices_bytes(o.indices_bytes), pal_size(o.pal_size) {
    o.indices = nullptr;
    o.indices_bytes = 0;
    o.pal_size = 0;
}

MutableSection::FlatData& MutableSection::FlatData::operator=(FlatData&& o) noexcept {
    if (this != &o) {
        delete[] indices;
        palette = std::move(o.palette);
        indices = o.indices;
        indices_bytes = o.indices_bytes;
        pal_size = o.pal_size;
        o.indices = nullptr;
        o.indices_bytes = 0;
        o.pal_size = 0;
    }
    return *this;
}

MutableSection::FlatData::~FlatData() { delete[] indices; }

MutableSection::~MutableSection() { clear(); }

MutableSection::MutableSection(MutableSection&& o) noexcept { moveFrom(std::move(o)); }

MutableSection& MutableSection::operator=(MutableSection&& o) noexcept {
    if (this != &o) { clear(); moveFrom(std::move(o)); }
    return *this;
}

uint16_t MutableSection::getBlock(int li) const {
    if (type == SectionType::AIR) return 0;

    if (type == SectionType::OVERFLOW) {
        return overflow[li];
    }

    // BITPACK4 or FLAT
    const int bpi = (type == SectionType::BITPACK4) ? 4 : 8;
    const uint32_t mask = (type == SectionType::BITPACK4) ? 0xF : 0xFF;
    const int shift = li * bpi;
    const int byte = shift >> 3;
    const int bit = shift & 7;
    const uint32_t raw = (flat->indices[byte] >> bit) |
                         (static_cast<uint32_t>(flat->indices[byte + 1]) << (8 - bit));
    return flat->palette[raw & mask];
}

void MutableSection::setBlock(int li, uint16_t id) {
    if (type == SectionType::AIR) {
        if (id == 0) return;
        type = SectionType::BITPACK4;
        flat = new FlatData{};
        flat->palette.push_back(0); // air
        flat->palette.push_back(id);
        flat->pal_size = 2;
        flat->indices_bytes = SEC_VOL * 4 / 8 + 1; // +1 for readBitpacked boundary
        flat->indices = new uint8_t[flat->indices_bytes]();
        writeBitpacked(li, 1, 4); // index 1 = id
        return;
    }

    if (type == SectionType::OVERFLOW) {
        uint16_t old = overflow[li];
        overflow[li] = id;
        if (old != id) checkOverflowDemote();
        return;
    }

    // BITPACK4 or FLAT
    const int bpi = (type == SectionType::BITPACK4) ? 4 : 8;
    const uint16_t max_pal = (type == SectionType::BITPACK4) ? 16 : 256;

    // Find block ID in palette
    int pal_idx = -1;
    for (int i = 0; i < flat->pal_size; ++i) {
        if (flat->palette[i] == id) { pal_idx = i; break; }
    }

    if (pal_idx == -1) {
        if (flat->pal_size >= max_pal) {
            promoteToFlatOrOverflow(id);
            setBlock(li, id);
            return;
        }
        pal_idx = flat->pal_size++;
        flat->palette.push_back(id);
    }

    writeBitpacked(li, pal_idx, bpi);
}

void MutableSection::fromBlocks(const uint16_t blocks[4096]) {
    clear();

    std::vector<uint16_t> unique;
    unique.reserve(257);
    unique.push_back(0); // air always at index 0

    for (int i = 0; i < SEC_VOL; ++i) {
        uint16_t bid = blocks[i];
        if (bid == 0) continue;
        bool found = false;
        for (uint16_t u : unique) {
            if (u == bid) { found = true; break; }
        }
        if (!found) {
            unique.push_back(bid);
            if (unique.size() > 256) break;
        }
    }

    if (unique.size() == 1) {
        type = SectionType::AIR;
        return;
    }

    if (unique.size() <= 16) {
        type = SectionType::BITPACK4;
        flat = new FlatData{};
        flat->palette = unique;
        flat->pal_size = flat->palette.size();
        flat->indices_bytes = (SEC_VOL * 4 + 7) / 8 + 1;
        flat->indices = new uint8_t[flat->indices_bytes]();
        for (int li = 0; li < SEC_VOL; ++li) {
            uint16_t bid = blocks[li];
            int idx = 0;
            for (size_t j = 0; j < flat->palette.size(); ++j) {
                if (flat->palette[j] == bid) { idx = j; break; }
            }
            writeBitpacked(li, idx, 4);
        }
    } else if (unique.size() <= 256) {
        type = SectionType::FLAT;
        flat = new FlatData{};
        flat->palette = unique;
        flat->pal_size = flat->palette.size();
        flat->indices_bytes = SEC_VOL + 1;
        flat->indices = new uint8_t[SEC_VOL + 1]();
        for (int li = 0; li < SEC_VOL; ++li) {
            uint16_t bid = blocks[li];
            int idx = 0;
            for (size_t j = 0; j < flat->palette.size(); ++j) {
                if (flat->palette[j] == bid) { idx = j; break; }
            }
            flat->indices[li] = static_cast<uint8_t>(idx);
        }
    } else {
        type = SectionType::OVERFLOW;
        overflow = new uint16_t[SEC_VOL];
        std::memcpy(overflow, blocks, sizeof(uint16_t) * SEC_VOL);
    }
}

void MutableSection::encodeToWire(std::vector<uint8_t> &buf) const {
    auto t0 = std::chrono::steady_clock::now();
    auto record = [&]() {
        /*auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        auto& m = Metrics::instance();
        m.encode_us_total += us;
        m.encode_count++;*/
    };
    if (type == SectionType::AIR) {
        writeU16(buf, 1);
        writeU16(buf, 0);
        writeU8(buf, 0);
        writeMetaMbToWire(buf);
        record();
        return;
    }

    if (type == SectionType::BITPACK4) {
        writeU16(buf, flat->pal_size);
        for (int i = 0; i < flat->pal_size; ++i)
            writeU16(buf, flat->palette[i]);
        writeU8(buf, 4);
        // Wire format: canonical size only (no +1 extra byte)
        const uint32_t wire_bytes = (SEC_VOL * 4 + 7) / 8; // 2048
        buf.insert(buf.end(), flat->indices, flat->indices + wire_bytes);
        writeMetaMbToWire(buf);
        record();
        return;
    }

    if (type == SectionType::FLAT) {
        writeU16(buf, flat->pal_size);
        for (int i = 0; i < flat->pal_size; ++i)
            writeU16(buf, flat->palette[i]);
        writeU8(buf, 8);
        const uint32_t wire_bytes = SEC_VOL;
        buf.insert(buf.end(), flat->indices, flat->indices + wire_bytes);
        writeMetaMbToWire(buf);
        record();
        return;
    }

    std::vector<uint16_t> pal;
    pal.reserve(257);
    pal.push_back(0);

    for (int i = 0; i < SEC_VOL; ++i) {
        uint16_t bid = overflow[i];
        if (bid == 0) continue;
        bool found = false;
        for (size_t j = 1; j < pal.size(); ++j) {
            if (pal[j] == bid) { found = true; break; }
        }
        if (!found) pal.push_back(bid);
    }

    writeU16(buf, pal.size());
    for (uint16_t p : pal) writeU16(buf, p);

    int bpi_val = 0;
    if (pal.size() > 1) {
        uint32_t tmp = pal.size() - 1;
        while (tmp > 0) { ++bpi_val; tmp >>= 1; }
    }
    writeU8(buf, static_cast<uint8_t>(bpi_val));

    if (bpi_val > 0) {
        const int total_bits = SEC_VOL * bpi_val;
        const size_t start = buf.size();
        buf.resize(start + (total_bits + 7) / 8, 0);
        uint8_t* dst = buf.data() + start;
        uint8_t byte = 0;
        int bit_idx = 0;

        for (int i = 0; i < SEC_VOL; ++i) {
            uint16_t bid = overflow[i];
            uint16_t idx = 0;
            if (bid != 0) {
                for (size_t j = 1; j < pal.size(); ++j) {
                    if (pal[j] == bid) { idx = j; break; }
                }
            }
            for (int b = 0; b < bpi_val; ++b) {
                byte |= static_cast<uint8_t>((idx & 1) << bit_idx);
                idx >>= 1;
                if (++bit_idx == 8) { *dst++ = byte; byte = 0; bit_idx = 0; }
            }
        }
        if (bit_idx != 0) *dst = byte;
    }
    writeMetaMbToWire(buf);
    record();
}

bool MutableSection::fromWire(Reader &r) {
    clear();
    auto t0 = std::chrono::steady_clock::now();
    auto record = [&]() {
        /*auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        auto& m = Metrics::instance();
        m.decode_us_total += us;
        m.decode_count++;*/
    };

    uint16_t psz = 0;
    if (!r.readU16(psz) || psz == 0) return false;
    std::vector<uint16_t> palette(psz);
    for (int i = 0; i < psz; ++i)
        if (!r.readU16(palette[i])) return false;

    uint8_t bpi = 0;
    if (!r.readU8(bpi)) return false;

    if (psz == 1 && palette[0] == 0 && bpi == 0) {
        type = SectionType::AIR;
        uint16_t mc = 0;
        if (!r.readU16(mc)) return false;
        if (!r.skip(static_cast<size_t>(mc) * 3)) return false;
        uint16_t mbc = 0;
        if (!r.readU16(mbc)) return false;
        if (!r.skip(static_cast<size_t>(mbc) * 6)) return false;
        record();
        return true;
    }

    std::vector<std::pair<uint16_t, uint8_t>> wire_meta;
    std::vector<std::pair<uint16_t, uint32_t>> wire_mb;

    if (psz <= 16 && bpi == 4) {
        type = SectionType::BITPACK4;
        flat = new FlatData{};
        flat->palette = std::move(palette);
        flat->pal_size = flat->palette.size();
        flat->indices_bytes = (SEC_VOL * 4 + 7) / 8 + 1;
        flat->indices = new uint8_t[flat->indices_bytes]();
        int total_bytes = (SEC_VOL * 4 + 7) / 8;
        if (r.pos + total_bytes > r.size) return false;
        std::memcpy(flat->indices, r.data + r.pos, total_bytes);
        r.pos += total_bytes;
    } else if (psz <= 256 && bpi == 8) {
        type = SectionType::FLAT;
        flat = new FlatData{};
        flat->palette = std::move(palette);
        flat->pal_size = flat->palette.size();
        flat->indices_bytes = SEC_VOL + 1;
        flat->indices = new uint8_t[SEC_VOL + 1]();
        if (r.pos + SEC_VOL > r.size) return false;
        std::memcpy(flat->indices, r.data + r.pos, SEC_VOL);
        r.pos += SEC_VOL;
    } else if (bpi == 0) {
        type = SectionType::BITPACK4;
        flat = new FlatData{};
        flat->palette = std::move(palette);
        flat->pal_size = flat->palette.size();
        flat->indices_bytes = (SEC_VOL * 4 + 7) / 8 + 1;
        flat->indices = new uint8_t[flat->indices_bytes]();
    } else {
        type = SectionType::OVERFLOW;
        overflow = new uint16_t[SEC_VOL];
        int total_bits = SEC_VOL * bpi;
        int total_bytes = (total_bits + 7) / 8;
        if (r.pos + total_bytes > r.size) return false;
        BitReader br{r.data + r.pos, 0};
        for (int li = 0; li < SEC_VOL; ++li) {
            uint32_t idx = br.read(bpi);
            if (idx >= psz) return false;
            overflow[li] = palette[idx];
        }
        r.pos += total_bytes;
        palette.clear();
    }

    uint16_t mc = 0;
    if (!r.readU16(mc)) return false;
    wire_meta.reserve(mc);
    for (int i = 0; i < mc; ++i) {
        uint16_t li = 0;
        uint8_t m = 0;
        if (!r.readU16(li) || !r.readU8(m)) return false;
        if (li < SEC_VOL) wire_meta.push_back({li, m});
    }

    uint16_t mbc = 0;
    if (!r.readU16(mbc)) return false;
    wire_mb.reserve(mbc);
    for (int i = 0; i < mbc; ++i) {
        uint16_t li = 0;
        uint32_t mbid = 0;
        if (!r.readU16(li) || !r.readU32(mbid)) return false;
        if (li < SEC_VOL) wire_mb.push_back({li, mbid});
    }

    for (auto& [li, m] : wire_meta) setMeta(li, m);
    for (auto& [li, mb] : wire_mb) setMultiblock(li, mb);
    record();
    return true;
}

uint8_t MutableSection::getMeta(int li) const {
    if (!meta_flat && meta_entries.empty()) return 0;
    if (meta_flat) return meta_flat[li];
    auto it = std::lower_bound(meta_entries.begin(), meta_entries.end(),
                               static_cast<uint16_t>(li),
                               [](const auto& p, uint16_t v) { return p.first < v; });
    if (it != meta_entries.end() && it->first == li) return it->second;
    return 0;
}

void MutableSection::setMeta(int li, uint8_t m) {
    if (m == 0) { removeMetaEntry(li); return; }
    if (meta_flat) { meta_flat[li] = m; return; }

    auto it = std::lower_bound(meta_entries.begin(), meta_entries.end(),
                               static_cast<uint16_t>(li),
                               [](const auto& p, uint16_t v) { return p.first < v; });

    if (it != meta_entries.end() && it->first == li) {
        it->second = m;
    } else {
        meta_entries.insert(it, {static_cast<uint16_t>(li), m});
        if (meta_entries.size() > SPARSE_THRESHOLD) flattenMeta();
    }
}

uint32_t MutableSection::getMultiblock(int li) const {
    if (!mb_flat && mb_entries.empty()) return 0;
    if (mb_flat) return mb_flat[li];
    auto it = std::lower_bound(mb_entries.begin(), mb_entries.end(),
                               static_cast<uint16_t>(li),
                               [](const auto& p, uint16_t v) { return p.first < v; });
    if (it != mb_entries.end() && it->first == li) return it->second;
    return 0;
}

void MutableSection::setMultiblock(int li, uint32_t mb_id) {
    if (mb_id == 0) { removeMbEntry(li); return; }
    if (mb_flat) { mb_flat[li] = mb_id; return; }

    auto it = std::lower_bound(mb_entries.begin(), mb_entries.end(),
                               static_cast<uint16_t>(li),
                               [](const auto& p, uint16_t v) { return p.first < v; });

    if (it != mb_entries.end() && it->first == li) {
        it->second = mb_id;
    } else {
        mb_entries.insert(it, {static_cast<uint16_t>(li), mb_id});
        if (mb_entries.size() > SPARSE_THRESHOLD) flattenMb();
    }
}

void MutableSection::writeMetaMbToWire(std::vector<uint8_t> &buf) const {
    if (meta_flat) {
        uint16_t count = 0;
        for (int i = 0; i < SEC_VOL; ++i)
            if (meta_flat[i] != 0) ++count;
        writeU16(buf, count);
        for (int i = 0; i < SEC_VOL; ++i)
            if (meta_flat[i] != 0) {
                writeU16(buf, static_cast<uint16_t>(i));
                writeU8(buf, meta_flat[i]);
            }
    } else {
        writeU16(buf, static_cast<uint16_t>(meta_entries.size()));
        for (auto& [li, m] : meta_entries) {
            writeU16(buf, li);
            writeU8(buf, m);
        }
    }

    if (mb_flat) {
        uint16_t count = 0;
        for (int i = 0; i < SEC_VOL; ++i)
            if (mb_flat[i] != 0) ++count;
        writeU16(buf, count);
        for (int i = 0; i < SEC_VOL; ++i)
            if (mb_flat[i] != 0) {
                writeU16(buf, static_cast<uint16_t>(i));
                writeU32(buf, mb_flat[i]);
            }
    } else {
        writeU16(buf, static_cast<uint16_t>(mb_entries.size()));
        for (auto& [li, mb] : mb_entries) {
            writeU16(buf, li);
            writeU32(buf, mb);
        }
    }
}

void MutableSection::writeBitpacked(int li, uint16_t idx, int bpi) const {
    const int shift = li * bpi;
    const int byte = shift >> 3;
    const int bit = shift & 7;
    const uint32_t mask = ((1u << bpi) - 1) << bit;
    flat->indices[byte] = static_cast<uint8_t>(
        (flat->indices[byte] & ~mask) | ((idx << bit) & mask));
    if (bit + bpi > 8) {
        flat->indices[byte + 1] = static_cast<uint8_t>(
            (flat->indices[byte + 1] & ~(mask >> 8)) | ((idx >> (8 - bit)) & (mask >> 8)));
    }
}

void MutableSection::promoteToFlatOrOverflow(uint16_t new_id) {
    uint16_t blocks[SEC_VOL];
    for (int li = 0; li < SEC_VOL; ++li)
        blocks[li] = getBlock(li);

    std::vector<uint16_t> unique;
    unique.reserve(257);
    unique.push_back(new_id);
    for (int li = 0; li < SEC_VOL; ++li) {
        bool found = false;
        for (uint16_t u : unique) {
            if (u == blocks[li]) { found = true; break; }
        }
        if (!found) {
            unique.push_back(blocks[li]);
            if (unique.size() > 256) break;
        }
    }

    if (unique.size() <= 256) {
        type = SectionType::FLAT;
        //Metrics::instance().promote_bitpack4_to_flat++;
        delete[] flat->indices;
        flat->indices_bytes = SEC_VOL + 1;
        flat->indices = new uint8_t[SEC_VOL + 1]();

        flat->palette.clear();
        flat->palette.push_back(0);
        for (uint16_t u : unique) {
            if (u != 0) flat->palette.push_back(u);
        }
        flat->pal_size = flat->palette.size();

        for (int li = 0; li < SEC_VOL; ++li) {
            uint16_t bid = blocks[li];
            int idx = 0;
            for (size_t j = 0; j < flat->palette.size(); ++j) {
                if (flat->palette[j] == bid) { idx = j; break; }
            }
            flat->indices[li] = static_cast<uint8_t>(idx);
        }
        return;
    }

    FlatData* old_flat = flat;
    flat = nullptr;
    type = SectionType::OVERFLOW;
    //Metrics::instance().promote_flat_to_overflow++;
    overflow = new uint16_t[SEC_VOL];
    std::memcpy(overflow, blocks, sizeof(uint16_t) * SEC_VOL);
    delete old_flat;
}

void MutableSection::checkOverflowDemote() {
    std::vector<uint16_t> unique;
    unique.reserve(257);
    unique.push_back(0);
    for (int i = 0; i < SEC_VOL; ++i) {
        uint16_t bid = overflow[i];
        if (bid == 0) continue;
        bool found = false;
        for (uint16_t u : unique) {
            if (u == bid) { found = true; break; }
        }
        if (!found) {
            unique.push_back(bid);
            if (unique.size() > 256) return;
        }
    }

    uint16_t* old_overflow = overflow;
    overflow = nullptr;
    type = SectionType::FLAT;
    //Metrics::instance().demote_overflow_to_flat++;
    flat = new FlatData{};
    flat->palette = unique;
    flat->pal_size = flat->palette.size();
    flat->indices_bytes = SEC_VOL + 1;
    flat->indices = new uint8_t[SEC_VOL + 1]();
    for (int li = 0; li < SEC_VOL; ++li) {
        uint16_t bid = old_overflow[li];
        int idx = 0;
        for (size_t j = 0; j < flat->palette.size(); ++j) {
            if (flat->palette[j] == bid) { idx = j; break; }
        }
        flat->indices[li] = static_cast<uint8_t>(idx);
    }
    delete[] old_overflow;
}

void MutableSection::flattenMeta() {
    meta_flat = new uint8_t[SEC_VOL]();
    for (auto& [li, m] : meta_entries)
        meta_flat[li] = m;
    meta_entries.clear();
    meta_entries.shrink_to_fit();
}

void MutableSection::flattenMb() {
    mb_flat = new uint32_t[SEC_VOL]();
    for (auto& [li, mb] : mb_entries)
        mb_flat[li] = mb;
    mb_entries.clear();
    mb_entries.shrink_to_fit();
}

void MutableSection::removeMetaEntry(int li) {
    if (meta_flat) {
        meta_flat[li] = 0;
        return;
    }
    auto it = std::lower_bound(meta_entries.begin(), meta_entries.end(),
                               static_cast<uint16_t>(li),
                               [](const auto& p, uint16_t v) { return p.first < v; });
    if (it != meta_entries.end() && it->first == li)
        meta_entries.erase(it);
}

void MutableSection::removeMbEntry(int li) {
    if (mb_flat) {
        mb_flat[li] = 0;
        return;
    }
    auto it = std::lower_bound(mb_entries.begin(), mb_entries.end(),
                               static_cast<uint16_t>(li),
                               [](const auto& p, uint16_t v) { return p.first < v; });
    if (it != mb_entries.end() && it->first == li)
        mb_entries.erase(it);
}

void MutableSection::clear() {
    delete flat; flat = nullptr;
    delete[] overflow; overflow = nullptr;
    delete[] meta_flat; meta_flat = nullptr;
    delete[] mb_flat; mb_flat = nullptr;
    meta_entries.clear();
    mb_entries.clear();
    type = SectionType::AIR;
}

void MutableSection::moveFrom(MutableSection &&o) {
    type = o.type; o.type = SectionType::AIR;
    flat = o.flat; o.flat = nullptr;
    overflow = o.overflow; o.overflow = nullptr;
    meta_entries = std::move(o.meta_entries);
    mb_entries = std::move(o.mb_entries);
    meta_flat = o.meta_flat; o.meta_flat = nullptr;
    mb_flat = o.mb_flat; o.mb_flat = nullptr;
}
