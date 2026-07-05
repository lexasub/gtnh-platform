#pragma once

// Palette-based section codec for Chunk ↔ LMDB serialization.
//
// Splits the 32³ block array into 8 sections of 16×16×16 and
// palette-compresses each section individually. Air sections (=uniform)
// cost 9 bytes instead of 24 KB. Typical terrain chunk: ~2 KB vs 192 KB.
//
// Disk format (all integers little-endian):
//   [magic: "GCHK" (4 bytes)] [ver: 1] [sec_cnt: 8]
//   sections[8]:
//     [palette_size: u16]
//     [palette: palette_size × u16]
//     [bits_per_index: u8; 0 = uniform section]
//     [indices: ceil(4096 × bits_per_index / 8) bytes]
//     [meta_count: u16]
//     [meta: meta_count × { local_idx: u16, meta: u8 }]
//     [mb_count: u16]
//     [mb: mb_count × { local_idx: u16, mb_id: u32 }]

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "../Chunk/Chunk.h"

constexpr int SEC_SZ = 16;
constexpr int SEC_VOL = SEC_SZ * SEC_SZ * SEC_SZ;
constexpr int SEC_CNT = 8;

constexpr uint32_t MAGIC = 0x4B484347; // "GCHK"


// Maps section index bit axis to origin: bit0=x, bit1=z, bit2=y
inline int sectionOrigin(int section, int axis) {
  return ((section >> axis) & 1) * SEC_SZ;
}

inline int localIndex(int lx, int ly, int lz) {
  return (ly << 8) | (lz << 4) | lx;
}

inline int chunkIndex(int x, int y, int z) { return (y << 10) | (z << 5) | x; }

struct BitWriter {
  std::vector<uint8_t> &buf;
  int bit_pos = 0;

  void write(uint16_t (&val)[SEC_VOL], int bits) {
    size_t start = buf.size();
    size_t total_bits = SEC_VOL * bits;
    buf.resize(start + (total_bits + 7) / 8, 0);  // один resize, zero-fill

    uint8_t* __restrict dst = buf.data() + start;
    uint8_t byte = 0;
    int bit_idx = 0;

    for (int j = 0; j < SEC_VOL; ++j) {
      uint16_t v = val[j];
      for (int i = 0; i < bits; ++i) {
        byte |= static_cast<uint8_t>((v & 1) << bit_idx);
        v >>= 1;
        if (++bit_idx == 8) {
          *dst++ = byte;
          byte = 0;
          bit_idx = 0;
        }
      }
    }
    if (bit_idx != 0)
      *dst = byte;
  }
};

struct BitReader {
  const uint8_t *data;
  int bit_pos = 0;

  uint32_t read(int bits) {
    uint32_t val = 0;
    for (int i = 0; i < bits; ++i) {
      if ((data[bit_pos / 8] >> (bit_pos % 8)) & 1)
        val |= (1u << i);
      ++bit_pos;
    }
    return val;
  }
};

// ─── wire-format helpers ──────────────────────────────────────────

inline void writeU8(std::vector<uint8_t> &buf, uint8_t v) { buf.push_back(v); }

inline void writeU16(std::vector<uint8_t> &buf, uint16_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void writeU32(std::vector<uint8_t> &buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

struct Reader {
  const uint8_t *data;
  size_t size;
  size_t pos = 0;

  bool readU8(uint8_t &v) {
    if (pos + 1 > size)
      return false;
    v = data[pos++];
    return true;
  }

  bool readU16(uint16_t &v) {
    if (pos + 2 > size)
      return false;
    v = static_cast<uint16_t>(data[pos]) |
        (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return true;
  }

  bool readU32(uint32_t &v) {
    if (pos + 4 > size)
      return false;
    v = static_cast<uint32_t>(data[pos]) |
        (static_cast<uint32_t>(data[pos + 1]) << 8) |
        (static_cast<uint32_t>(data[pos + 2]) << 16) |
        (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
  }

  bool skip(size_t n) {
    if (pos + n > size)
      return false;
    pos += n;
    return true;
  }
};

// ─── encoder ──────────────────────────────────────────────────────

void encodeSection(const Chunk &chunk, int section,
                  std::vector<uint8_t> &buf);

void encodeChunk(const Chunk &chunk, std::vector<uint8_t> &buf);

// ─── section offset table ─────────────────────────────────────────
// Scans the compressed buffer and records byte offset for each of the 8
// sections. Enables O(1) jump to any section for lazy decoding.

inline bool buildSectionOffsets(const uint8_t *data, size_t size,
                                uint32_t offsets[8]) {
  Reader r{data, size, 0};
  uint32_t magic = 0;
  if (!r.readU32(magic) || magic != MAGIC)
    return false;
  uint8_t ver = 0;
  if (!r.readU8(ver) || ver < 1)
    return false;
  uint8_t sec_cnt = 0;
  if (!r.readU8(sec_cnt) || sec_cnt != SEC_CNT)
    return false;

  for (int s = 0; s < SEC_CNT; ++s) {
    offsets[s] = r.pos;

    uint16_t psz = 0;
    if (!r.readU16(psz) || psz == 0)
      return false;
    if (r.pos + static_cast<size_t>(psz) * 2 > r.size)
      return false;
    r.pos += psz * 2;

    uint8_t bpi = 0;
    if (!r.readU8(bpi))
      return false;

    if (bpi > 0) {
      int total_bits = SEC_VOL * bpi;
      r.pos += (total_bits + 7) / 8;
    }

    uint16_t mc = 0;
    if (!r.readU16(mc))
      return false;
    if (r.pos + static_cast<size_t>(mc) * 3 > r.size)
      return false;
    r.pos += mc * 3;

    uint16_t mbc = 0;
    if (!r.readU16(mbc))
      return false;
    if (r.pos + static_cast<size_t>(mbc) * 6 > r.size)
      return false;
    r.pos += mbc * 6;
  }
  return true;
}

// ─── single-section decoder ───────────────────────────────────────
// Decodes one palette-compressed section into caller-provided 4096-element
// arrays. blocks_out/meta_out/mb_out use section-local indexing: (ly*256 +
// lz*16 + lx). section_offset is obtained from buildSectionOffsets().

inline bool decodeSingleSection(const uint8_t *data, size_t size, int section,
                                uint16_t *blocks_out, uint8_t *meta_out,
                                uint32_t *mb_out, uint32_t section_offset) {
  (void)section;
  Reader r{data, size, section_offset};

  std::memset(blocks_out, 0, SEC_VOL * sizeof(uint16_t));
  std::memset(meta_out, 0, SEC_VOL * sizeof(uint8_t));
  std::memset(mb_out, 0, SEC_VOL * sizeof(uint32_t));

  uint16_t psz = 0;
  if (!r.readU16(psz) || psz == 0)
    return false;
  std::vector<uint16_t> palette(psz);
  for (int i = 0; i < psz; ++i) {
    if (!r.readU16(palette[i]))
      return false;
  }

  uint8_t bpi = 0;
  if (!r.readU8(bpi))
    return false;

  if (bpi > 0) {
    int total_bits = SEC_VOL * bpi;
    int total_bytes = (total_bits + 7) / 8;
    if (r.pos + total_bytes > r.size)
      return false;

    BitReader br{r.data + r.pos, 0};
    for (int li = 0; li < SEC_VOL; ++li) {
      uint32_t pal_idx = br.read(bpi);
      if (pal_idx >= psz)
        return false;
      blocks_out[li] = palette[pal_idx];
    }
    r.pos += total_bytes;
  } else {
    uint16_t uniform = palette[0];
    for (int li = 0; li < SEC_VOL; ++li)
      blocks_out[li] = uniform;
  }

  uint16_t mc = 0;
  if (!r.readU16(mc))
    return false;
  for (int i = 0; i < mc; ++i) {
    uint16_t li = 0;
    uint8_t m = 0;
    if (!r.readU16(li) || !r.readU8(m))
      return false;
    if (li < SEC_VOL)
      meta_out[li] = m;
  }

  uint16_t mbc = 0;
  if (!r.readU16(mbc))
    return false;
  for (int i = 0; i < mbc; ++i) {
    uint16_t li = 0;
    uint32_t mbid = 0;
    if (!r.readU16(li) || !r.readU32(mbid))
      return false;
    if (li < SEC_VOL)
      mb_out[li] = mbid;
  }
  return true;
}

// ─── decoder ──────────────────────────────────────────────────────

inline bool decodeChunk(const uint8_t *data, size_t size, Chunk &chunk) { //TODO profile loading cart and fix if needed
  std::memset(&chunk, 0, sizeof(Chunk));

  Reader r{data, size, 0};

  uint32_t magic = 0;
  if (!r.readU32(magic) || magic != MAGIC)
    return false;
  uint8_t ver = 0;
  if (!r.readU8(ver) || ver < 1)
    return false;
  uint8_t sec_cnt = 0;
  if (!r.readU8(sec_cnt) || sec_cnt != SEC_CNT)
    return false;

  for (int s = 0; s < SEC_CNT; ++s) {
    int ox = sectionOrigin(s, 0);
    int oy = sectionOrigin(s, 2);
    int oz = sectionOrigin(s, 1);

    uint16_t psz = 0;
    if (!r.readU16(psz) || psz == 0)
      return false;
    std::vector<uint16_t> palette(psz);
    for (int i = 0; i < psz; ++i) {
      if (!r.readU16(palette[i]))
        return false;
    }

    uint8_t bpi = 0;
    if (!r.readU8(bpi))
      return false;

    if (bpi > 0) {
      int total_bits = SEC_VOL * bpi;
      int total_bytes = (total_bits + 7) / 8;
      if (r.pos + total_bytes > r.size)
        return false;

      BitReader br{r.data + r.pos, 0};
      for (int li = 0; li < SEC_VOL; ++li) {
        uint32_t pal_idx = br.read(bpi);
        if (pal_idx >= psz)
          return false;

        uint16_t bid = palette[pal_idx];
        int lx = (ox + li) & 0xF;
        int lz = (oy + (li >> 4)) & 0xF;
        int ly = (oz + (li >> 8)) & 0xF;
        chunk.GetBlock(lx, ly, lz) = bid;
      }
      r.pos += total_bytes;
    } else {
      uint16_t uniform_bid = palette[0];
      for (int ly = oy; ly < SEC_SZ; ++ly)
        for (int lz = oz; lz < SEC_SZ; ++lz)
          for (int lx = ox; lx < SEC_SZ; ++lx)
            chunk.GetBlock( lx,  ly,  lz) = uniform_bid;
    }

    uint16_t mc = 0;
    if (!r.readU16(mc))
      return false;
    for (int i = 0; i < mc; ++i) {
      uint16_t li = 0;
      uint8_t meta = 0;
      if (!r.readU16(li) || !r.readU8(meta))
        return false;
      int lx = li & 0xF;
      int lz = (li >> 4) & 0xF;
      int ly = (li >> 8) & 0xF;
      chunk.meta[chunkIndex(ox + lx, oy + ly, oz + lz)] = meta;
    }

    uint16_t mbc = 0;
    if (!r.readU16(mbc))
      return false;
    for (int i = 0; i < mbc; ++i) {
      uint16_t li = 0;
      uint32_t mb_id = 0;
      if (!r.readU16(li) || !r.readU32(mb_id))
        return false;
      int lx = li & 0xF;
      int lz = (li >> 4) & 0xF;
      int ly = (li >> 8) & 0xF;
      chunk.multiblock[chunkIndex(ox + lx, oy + ly, oz + lz)] = mb_id;
    }
  }

  return true;
}
