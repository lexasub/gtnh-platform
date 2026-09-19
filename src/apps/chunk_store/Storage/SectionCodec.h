#pragma once

// Palette-based wire format helpers for section ↔ LMDB serialization.
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
#include <vector>

constexpr int SEC_SZ = 16;
constexpr int SEC_VOL = SEC_SZ * SEC_SZ * SEC_SZ;
constexpr int SEC_CNT = 8;

constexpr uint32_t MAGIC = 0x4B484347; // "GCHK"

inline int sectionOrigin(int section, int axis) {
  return ((section >> axis) & 1) * SEC_SZ;
}

inline int localIndex(int lx, int ly, int lz) {
  return (ly << 8) | (lz << 4) | lx;
}

inline int chunkIndex(int x, int y, int z) { return (y << 10) | (z << 5) | x; }

struct BitReader {
  const uint8_t *data;
  int bit_pos = 0;

  uint32_t read(int bits);
};

void writeU8(std::vector<uint8_t> &buf, uint8_t v);

void writeU16(std::vector<uint8_t> &buf, uint16_t v);

void writeU32(std::vector<uint8_t> &buf, uint32_t v);

struct Reader {
  const uint8_t *data;
  size_t size;
  size_t pos = 0;

  bool readU8(uint8_t &v);

  bool readU16(uint16_t &v);

  bool readU32(uint32_t &v);

  bool skip(size_t n);
};
