// MutableChunk.h — Palette-native chunk representation.
//
// Chunk хранится ТОЛЬКО в сжатом виде (palette + indices).
// setBlock патчит palette/indices напрямую — без decode/encode.
// При отправке клиенту — encodeToWire() → стандартный wire-формат.
//
// Wire-протокол не меняется.
//
// Section types:
//   AIR      — все блоки = 0. 0 heap.
//   BITPACK4 — palette ≤ 16. indices packed 4-bit = 2KB heap.
//   FLAT     — palette ≤ 256. indices[4096] uint8_t = 4KB heap.
//   OVERFLOW — palette > 256 (редко). blocks[4096] uint16_t = 8KB heap.
//
// Promotion chain:
//   AIR → BITPACK4 [setBlock с id ≠ 0]
//   BITPACK4 → FLAT [palette > 16]
//   FLAT → OVERFLOW [palette > 256, редко в GTNH]
//
// Demotion chain:
//   OVERFLOW → FLAT [palette ≤ 256]
//   FLAT/BITPACK4 → AIR [все блоки = 0]
//
// Sparse meta/multiblock: sorted vector + binary search.
// Threshold 256 → flat array fallback.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../SectionCodec.h"
#include "MutableSection.h"


// ═══════════════════════════════════════════════════════════════
// MutableChunk
// ═══════════════════════════════════════════════════════════════

struct MutableChunk {
    MutableSection sections[SEC_CNT];

    // ─── Block access (chunk-local x,y,z → section → local_idx) ─

    [[nodiscard]] uint16_t getBlock(int x, int y, int z) const;

    void setBlock(int x, int y, int z, uint16_t id);

    // ─── Sparse meta/multiblock ──────────────────────────────────

    [[nodiscard]] uint8_t getMeta(int x, int y, int z) const;

    void setMeta(int x, int y, int z, uint8_t m);

    [[nodiscard]] uint32_t getMultiblock(int x, int y, int z) const;

    void setMultiblock(int x, int y, int z, uint32_t mb_id);

    // ─── Bulk write (world gen) ──────────────────────────────────

    static MutableChunk fromBlocks(const uint16_t blocks[SEC_VOL * SEC_CNT]);

    // ─── Wire serialization ──────────────────────────────────────

    void encodeToWire(std::vector<uint8_t>& buf) const;

    // ─── Decode from wire ────────────────────────────────────────

    bool fromWire(const uint8_t* data, size_t size);
};
