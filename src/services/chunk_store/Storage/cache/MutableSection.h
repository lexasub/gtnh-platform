#ifndef MUTABLESECTION_H
#define MUTABLESECTION_H


#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../SectionCodec.h"

enum class SectionType : uint8_t { AIR, BITPACK4, FLAT, OVERFLOW };

class MutableSection {
public:
    SectionType type = SectionType::AIR;

    // --- FLAT/BITPACK4 ---
    struct FlatData {
        std::vector<uint16_t> palette;   // [id, id, ...]
        uint8_t* indices = nullptr;      // BITPACK4: packed 4-bit. FLAT: 1 byte/entry.
        uint32_t indices_bytes = 0;      // allocated size
        uint16_t pal_size = 0;

        ~FlatData();

        FlatData() = default;
        FlatData(const FlatData&) = delete;
        FlatData& operator=(const FlatData&) = delete;
        FlatData(FlatData&& o) noexcept;
        FlatData& operator=(FlatData&& o) noexcept;
    };

    FlatData* flat = nullptr;

    // --- OVERFLOW ---
    uint16_t* overflow = nullptr; // blocks[4096] uint16_t

    // --- Sparse meta/multiblock ---
    // Sorted by local_idx → binary search O(log N).
    // Threshold 256 → flat array fallback.
    static constexpr int SPARSE_THRESHOLD = 256;

    std::vector<std::pair<uint16_t, uint8_t>> meta_entries; // (local_idx, meta)
    std::vector<std::pair<uint16_t, uint32_t>> mb_entries;  // (local_idx, mb_id)
    uint8_t* meta_flat = nullptr;    // flat fallback: 4096 bytes
    uint32_t* mb_flat = nullptr;     // flat fallback: 4096 × 4 bytes

    // ─── Lifecycle ───────────────────────────────────────────────

    MutableSection() = default;
    ~MutableSection();

    MutableSection(const MutableSection&) = delete;
    MutableSection& operator=(const MutableSection&) = delete;

    MutableSection(MutableSection&& o) noexcept;
    MutableSection& operator=(MutableSection&& o) noexcept;

    // ─── Type checks ────────────────────────────────────────────

    [[nodiscard]] bool isAir() const { return type == SectionType::AIR; }
    [[nodiscard]] SectionType getType() const { return type; }

    // ─── Block access ───────────────────────────────────────────

    [[nodiscard]] uint16_t getBlock(int li) const;

    void setBlock(int li, uint16_t id);

    // ─── Bulk write (world gen) ──────────────────────────────────

    void fromBlocks(const uint16_t blocks[SEC_VOL]);

    // ─── Wire serialization ──────────────────────────────────────

    void encodeToWire(std::vector<uint8_t>& buf) const;

    // ─── Decode from wire ────────────────────────────────────────

    bool fromWire(Reader& r);

    // ─── Sparse meta/multiblock ──────────────────────────────────

    [[nodiscard]] uint8_t getMeta(int li) const;

    void setMeta(int li, uint8_t m);

    [[nodiscard]] uint32_t getMultiblock(int li) const;

    void setMultiblock(int li, uint32_t mb_id);

private:
    // ─── Helpers ─────────────────────────────────────────────────

    void writeMetaMbToWire(std::vector<uint8_t>& buf) const;

    void writeBitpacked(int li, uint16_t idx, int bpi) const;

    void promoteToFlatOrOverflow(uint16_t new_id = 0);

    void checkOverflowDemote();

    void flattenMeta();

    void flattenMb();

    void removeMetaEntry(int li);

    void removeMbEntry(int li);

    void clear();

    void moveFrom(MutableSection&& o);
};



#endif //MUTABLESECTION_H
