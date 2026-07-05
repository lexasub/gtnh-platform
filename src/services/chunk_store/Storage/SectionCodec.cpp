#include "SectionCodec.h"

#include <array>
#include <emmintrin.h>

struct MetaEntry { uint16_t local_idx; uint8_t meta; };
struct MbEntry  { uint16_t local_idx; uint32_t mb_id; };

#include <algorithm>
#include <array>

void encodeSection(const Chunk &chunk, int section,
                   std::vector<uint8_t> &buf) {
    int ox = sectionOrigin(section, 0);
    int oy = sectionOrigin(section, 2);
    int oz = sectionOrigin(section, 1);

    std::vector<uint16_t> palette;
    palette.reserve(32);

    std::vector<MetaEntry> meta_entries;
    std::vector<MbEntry>   mb_entries;
    meta_entries.reserve(64);
    mb_entries.reserve(16);

    for (int ly = 0; ly < SEC_SZ; ++ly) {//TODO verify that 1 << 5 is valid for this case
        const uint8_t* __restrict meta_row = chunk.meta.data() + ((oy + ly) << 10 | oz << 5 | ox); // + 1 << 5 in cycle
        const uint32_t* __restrict multiblock_row = chunk.multiblock.data() + ((oy + ly) << 10 | oz << 5 | ox); // + 1 << 5 in cycle

        int idx = (ly << 8); // + 1 << 4 in cycle

        for (int lz = 0; lz < SEC_SZ; ++lz) {
            /*__builtin_prefetch( // подсказываем префетчеру загрузить еще пачку //may be add prefetchers
                reinterpret_cast<const char*>(src_row + 64), // +64 uint16_t = +128 bytes //it's jump to lz + 1 for this section
                0, 3 // read, high temporal locality
            );*/
            for (int lx = 0; lx < SEC_SZ; ++lx) {
                int li = idx + lx;

                uint8_t m = meta_row[lx];
                if (m != 0) [[unlikely]]
                    meta_entries.push_back({static_cast<uint16_t>(li), m});

                uint32_t mb = multiblock_row[lx];
                if (mb != 0) [[unlikely]]
                    mb_entries.push_back({static_cast<uint16_t>(li), mb});
            }
            meta_row += 1 << 5;
            multiblock_row += 1 << 5;
            idx += 1 << 4;
        }
    }
    // --- Только блоки и палитра ---
    // Линейный поиск в palette — данные в L1, нет random access

    // 1. Секция 16³ = 4096 блоков = 8 KB. Копируем в L1-буфер на стеке,
    //    чтобы убить cache misses от chunk.blocks (который 32³*2 = 64 KB,
    //    не влезает в L1, и ly даёт stride 2048 байт).
    alignas(64) uint16_t local_blocks[SEC_VOL];
    for (int ly = 0; ly < SEC_SZ; ++ly) {
        const uint16_t* __restrict src_row = chunk.blocks.data() + ((oy + ly) << 10 | oz << 5 | ox); // + 1 << 5 in cycle

        uint16_t* __restrict dst_row = local_blocks + (ly << 8); // + 1 << 4 in cycle

        for (int lz = 0; lz < SEC_SZ; ++lz) {
            __builtin_prefetch( // подсказываем префетчеру загрузить еще пачку
                reinterpret_cast<const char*>(src_row + 64), // +64 uint16_t = +128 bytes //it's jump to lz + 1 for this section
                0, 3 // read, high temporal locality
            );
            std::memcpy(dst_row, src_row, SEC_SZ * sizeof(uint16_t));
            src_row += 1 << 5;
            dst_row += 1 << 4;
        }
    }

    // 2. Палитра на стеке — 512 байт, L1. Branchless find.
    alignas(64) uint16_t local_pal[256];
    // Предполагаем: bid никогда не равен 0xFFFF
    // Инициализируем local_pal[0..255] = 0xFFFF (sentinel)
    std::memset(local_pal, 0xFF, sizeof(local_pal));
    local_pal[0] = local_blocks[0];

    local_blocks[0] = 0;
    uint16_t pal_size = 1;
    int li = 1;
    // Phase 1: scalar, pal_size 1..3. SSE latency не окупается.
    for (; li < SEC_VOL && pal_size < 4; ++li) {
        uint16_t bid = local_blocks[li];
        uint16_t idx = (local_pal[0] == bid) ? 0
             : (local_pal[1] == bid) ? 1
             : (local_pal[2] == bid) ? 2
             : pal_size;
        // pal_size < 4, так else-if достаточно

        local_pal[pal_size] = bid;
        pal_size += (idx == pal_size);
        local_blocks[li] = idx;
    }

    // Phase 2: SSE, одна загрузка, pal_size 4..16
    for (; li < SEC_VOL && pal_size <= 16; ++li) {
        uint16_t bid = local_blocks[li];
        __m128i target = _mm_set1_epi16(bid);
        __m128i p0 = _mm_load_si128(reinterpret_cast<__m128i*>(local_pal));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi16(p0, target));

        // ВАЖНО: pal_size*2 может быть 32, 1u<<32 — UB. Используем 1ull.
        uint32_t valid = (pal_size == 16) ? 0xFFFFFFFFu
                                         : static_cast<uint32_t>((1ull << (pal_size * 2)) - 1);
        mask &= valid;

        uint16_t idx;
        if (mask == 0) [[unlikely]] {
            idx = pal_size;
        } else [[likely]] {
            idx = __builtin_ctz(static_cast<unsigned>(mask)) >> 1;
        }

        local_pal[pal_size] = bid;
        pal_size += (idx == pal_size);
        local_blocks[li] = idx;
    }

    // Phase 3: SSE, две загрузки, pal_size > 16
    for (; li < SEC_VOL; ++li) {
        uint16_t bid = local_blocks[li];
        __m128i target = _mm_set1_epi16(bid);

        __m128i p0 = _mm_load_si128(reinterpret_cast<__m128i*>(local_pal));
        __m128i p1 = _mm_load_si128(reinterpret_cast<__m128i*>(local_pal) + 1);

        int m0 = _mm_movemask_epi8(_mm_cmpeq_epi16(p0, target));
        int m1 = _mm_movemask_epi8(_mm_cmpeq_epi16(p1, target));

        uint32_t mask = (static_cast<uint32_t>(static_cast<uint16_t>(m1)) << 16)
                      | static_cast<uint16_t>(m0);

        uint16_t idx;
        if (mask == 0) [[unlikely]] {
            idx = pal_size;
        } else [[likely]] {
            idx = __builtin_ctz(mask) >> 1;
        }

        local_pal[pal_size] = bid;
        pal_size += (idx == pal_size);
        local_blocks[li] = idx;
    }

    // 3. Одна запись из L1 в heap — за пределами горячего пути
    palette.assign(&local_pal[0], &local_pal[0] + pal_size);

    // --- Сериализация (без изменений) ---
    uint16_t psz = static_cast<uint16_t>(palette.size());
    writeU16(buf, psz);
    for (auto bid : palette) writeU16(buf, bid);

    int bpi_val = 0;
    if (psz > 1) {
        uint32_t tmp = psz - 1;
        while (tmp > 0) { ++bpi_val; tmp >>= 1; }
    }
    auto bpi = static_cast<uint8_t>(bpi_val);
    writeU8(buf, bpi);

    if (bpi > 0) {
        BitWriter bw{buf, static_cast<int>(buf.size()) * 8};
        bw.write(local_blocks, bpi);
    }

    uint16_t mc = static_cast<uint16_t>(meta_entries.size());
    writeU16(buf, mc);
    for (auto &e : meta_entries) {
        writeU16(buf, e.local_idx);
        writeU8(buf, e.meta);
    }

    uint16_t mbc = static_cast<uint16_t>(mb_entries.size());
    writeU16(buf, mbc);
    for (auto &e : mb_entries) {
        writeU16(buf, e.local_idx);
        writeU32(buf, e.mb_id);
    }
}


void encodeChunk(const Chunk &chunk, std::vector<uint8_t> &buf) {
  //TODO may be need cache encoded chunk (also we don't want encoded and not encoded in cache (mem X 2)
  buf.clear();
  writeU32(buf, MAGIC);
  writeU8(buf, 1);
  writeU8(buf, SEC_CNT);
  for (int s = 0; s < SEC_CNT; ++s)
    encodeSection(chunk, s, buf);
}