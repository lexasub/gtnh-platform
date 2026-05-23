#pragma once

#include <cstdint>
#include <optional>
#include <functional>
#include <bit>
#include <type_traits>

// Lock-free 4-way set-associative Clock Cache using DCAS (CMPXCHG16B).
//
// Each 128-bit atomic slot packs (int64_t key, uintptr_t value) so the pair
// is always read/written atomically — no race between separate stores.
//
// 4 slots per set fit in one 64-byte cache line → get() checks all 4 via a
// single cache-line fill. put() evicts within the set using CLOCK hand.
//
// Ref bit (kRefBit = 1ULL<<63) lives in the VALUE half (lower 64 bits).
// User-space pointers on x86-64 use only 48 canonical bits; bit 63 is
// always 0, making it safe for pointer tagging.
//
// Key 0 is RESERVED as the empty-slot sentinel. Callers must not store
// entries with key == 0.
//
// Eviction callback (on_evict) is called when a slot overwrites an existing
// non-empty entry — on pass-1 (same-key replace), pass-3 (eviction), erase,
// and clear(). The callback receives the old value (uintptr_t). Default
// no-op. Set via set_on_evict().
//
// Why raw __atomic_* builtins instead of std::atomic<__int128>?
//   GCC's std::atomic<__int128> delegates to libatomic even with -mcx16.
//   __atomic_* builtins inline to lock cmpxchg16b directly.
template <typename Value, size_t Capacity>
class ClockCache {
    static_assert(std::is_trivially_copyable_v<Value>);
    static_assert(sizeof(Value) <= sizeof(uintptr_t),
                  "Value must fit in uintptr_t (8 bytes on x86-64)");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of two");

    static constexpr size_t kAssoc  = 4;           // ways per set
    static constexpr size_t kSets   = Capacity / kAssoc;
    static constexpr size_t kCL     = 64;          // cache line

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    using PackedSlot = __int128;
#pragma GCC diagnostic pop
    static constexpr uintptr_t kRefBit = 1ULL << 63;

    // --- Raw __atomic_* helpers ---
    static PackedSlot load(PackedSlot const* p) noexcept {
        PackedSlot v;
        __atomic_load(p, &v, __ATOMIC_ACQUIRE);
        return v;
    }
    static void store(PackedSlot* p, PackedSlot v) noexcept {
        __atomic_store(p, &v, __ATOMIC_RELEASE);
    }
    static bool cas(PackedSlot* p, PackedSlot* exp, PackedSlot des) noexcept {
        return __atomic_compare_exchange_n(p, exp, des, false,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
    static bool cas_relaxed(PackedSlot* p, PackedSlot* exp,
                            PackedSlot des) noexcept {
        return __atomic_compare_exchange_n(p, exp, des, false,
            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }

    // --- Bit layout ---
    static PackedSlot pack(int64_t key, uintptr_t val) noexcept {
        auto hi = static_cast<PackedSlot>(static_cast<uint64_t>(key)) << 64;
        auto lo = static_cast<PackedSlot>(val | kRefBit);
        return hi | lo;
    }
    static int64_t   unpack_key(PackedSlot s) noexcept { return static_cast<int64_t>(s >> 64); }
    static uintptr_t unpack_val(PackedSlot s) noexcept { return static_cast<uintptr_t>(s) & ~kRefBit; }
    static bool      has_ref(PackedSlot s) noexcept { return static_cast<uintptr_t>(s) & kRefBit; }
    static PackedSlot set_ref(PackedSlot s) noexcept   { return s | static_cast<PackedSlot>(kRefBit); }
    static PackedSlot clear_ref(PackedSlot s) noexcept { return s & ~static_cast<PackedSlot>(kRefBit); }

    static size_t set_idx(int64_t key) noexcept {
        // Fold all 64 bits into the low log2(kSets) bits.
        // makeKey packs chunk coords as (cx<<42)|(cy<<21)|cz, giving each field
        // a 21-bit range.  The low bits are constant (0x200000 bias starts at
        // bit 21) — multiplicative hashing alone can't spread high-bit variation
        // into the low index bits when kSets is small (e.g. 256).  XOR folding
        // cascades every bit into the result.
        uint64_t k = static_cast<uint64_t>(key);
        k ^= k >> 32;
        k ^= k >> 16;
        k ^= k >> 8;
        return k & (kSets - 1);
    }

    // 4 slots per set, 16 bytes each = 64 bytes = one cache line
    struct alignas(kCL) Set {
        PackedSlot slot[kAssoc];
    };

    // Per-set CLOCK hand (0..kAssoc-1).  Relaxed atomic access — the hand is
    // purely a heuristic scan-start, not a correctness-critical lock.
    // Multiple put() threads racing on the same set may observe a stale hand
    // (worst case: scan starts slightly behind, catches up on retry).
    static uint8_t load_hand(uint8_t const* p) noexcept {
        uint8_t v;
        __atomic_load(p, &v, __ATOMIC_RELAXED);
        return v;
    }
    static void store_hand(uint8_t* p, uint8_t v) noexcept {
        __atomic_store(p, &v, __ATOMIC_RELAXED);
    }

    using EvictFn = std::function<void(uintptr_t)>;

public:
    ClockCache() noexcept = default;

    void set_on_evict(EvictFn fn) noexcept { on_evict_ = fn; }

    // Branchless get: loads all 4 slots (same cache line), selects match
    // via arithmetic mask — zero branches in the hot path, a single branch
    // on the cold miss path. On hit, fire-and-forget CAS to set ref bit.
    std::optional<Value> get(int64_t key) noexcept {
        Set& s = sets_[set_idx(key)];

        // All 4 slots sit in one 64-byte cache line — load them all.
        PackedSlot v0 = load(&s.slot[0]);
        PackedSlot v1 = load(&s.slot[1]);
        PackedSlot v2 = load(&s.slot[2]);
        PackedSlot v3 = load(&s.slot[3]);

        // Compares produce 0/1, negation (0 - x) maps to 0/~0 mask.
        uint64_t m0 = 0 - static_cast<uint64_t>(unpack_key(v0) == key);
        uint64_t m1 = 0 - static_cast<uint64_t>(unpack_key(v1) == key);
        uint64_t m2 = 0 - static_cast<uint64_t>(unpack_key(v2) == key);
        uint64_t m3 = 0 - static_cast<uint64_t>(unpack_key(v3) == key);

        // Miss — single branch on the cold path.
        uint64_t any = m0 | m1 | m2 | m3;
        if (!any) [[unlikely]]
            return std::nullopt;

        // OR-mask the value from whichever slot matched.
        uintptr_t val = 0;
        val |= unpack_val(v0) & m0;
        val |= unpack_val(v1) & m1;
        val |= unpack_val(v2) & m2;
        val |= unpack_val(v3) & m3;

        // Fire-and-forget ref-bit CAS on the matching slot.
        PackedSlot vs[4] = {v0, v1, v2, v3};
        unsigned idx = static_cast<unsigned>(__builtin_ctzll(any));
        cas_relaxed(&s.slot[idx], &vs[idx], set_ref(vs[idx]));

        return std::bit_cast<Value>(val);
    }

    // Insert (key, value). Load-all-4-once + CLOCK hand eviction.
    //
    //   Pass 1: same key   → branchless match, CAS overwrite.
    //   Pass 2: empty slot → CAS take it (no load needed).
    //   Pass 3: CLOCK scan from hand for ref=0, CAS evict + write, advance hand.
    //   Pass 4: clear ref bit at hand only, advance hand, retry.
    //
    // Per-set clock hand rotates evictions so hot entries (ref=1 from get())
    // survive until the hand wraps back around — true second-chance CLOCK.
    void put(int64_t key, Value value) noexcept {
        PackedSlot desired = pack(key, std::bit_cast<uintptr_t>(value));
        size_t si = set_idx(key);
        Set& s = sets_[si];

        for (;;) {
            // Load all 4 slots once per iteration (one cache-line fill).
            PackedSlot v0 = load(&s.slot[0]);
            PackedSlot v1 = load(&s.slot[1]);
            PackedSlot v2 = load(&s.slot[2]);
            PackedSlot v3 = load(&s.slot[3]);

            // --- Pass 1: same key (branchless) ---
            uint64_t m0 = 0 - static_cast<uint64_t>(unpack_key(v0) == key);
            uint64_t m1 = 0 - static_cast<uint64_t>(unpack_key(v1) == key);
            uint64_t m2 = 0 - static_cast<uint64_t>(unpack_key(v2) == key);
            uint64_t m3 = 0 - static_cast<uint64_t>(unpack_key(v3) == key);
            uint64_t same = m0 | m1 | m2 | m3;

            if (same) [[unlikely]] {
                unsigned idx = static_cast<unsigned>(__builtin_ctzll(same));
                PackedSlot vs[4] = {v0, v1, v2, v3};
                if (cas(&s.slot[idx], &vs[idx], desired)) [[likely]] {
                    if (on_evict_)
                        on_evict_(unpack_val(vs[idx]));
                    return;
                }
                continue;  // CAS failed — reload and retry.
            }

            // --- Pass 2: empty slot (CAS expects all-zero) ---
            for (size_t i = 0; i < kAssoc; ++i) {
                PackedSlot empty{};
                if (cas(&s.slot[i], &empty, desired)) [[likely]]
                    return;
            }

            // --- Pass 3: CLOCK scan from hand for ref=0 ---
            uint8_t h = load_hand(&hands_[si]);
            bool found = false;
            for (size_t i = 0; i < kAssoc; ++i) {
                unsigned idx = static_cast<unsigned>((h + i) & (kAssoc - 1));
                PackedSlot vs = (idx == 0) ? v0 : (idx == 1) ? v1
                                : (idx == 2) ? v2 : v3;
                if (!has_ref(vs)) {
                    PackedSlot old = vs;
                    if (cas(&s.slot[idx], &old, desired)) [[likely]] {
                        store_hand(&hands_[si],
                            static_cast<uint8_t>((idx + 1) & (kAssoc - 1)));
                        if (on_evict_ && unpack_key(old) != 0)
                            on_evict_(unpack_val(old));
                        return;
                    }
                    found = true;  // CAS failed — reload and retry.
                    break;
                }
            }
            if (found)
                continue;

            // --- Pass 4: clear ref at hand, advance hand, retry ---
            PackedSlot vh = load(&s.slot[h]);
            cas_relaxed(&s.slot[h], &vh, clear_ref(vh));
            store_hand(&hands_[si],
                static_cast<uint8_t>((h + 1) & (kAssoc - 1)));
        }
    }

    // Branchless erase: same load-all-4 + mask technique as get().
    void erase(int64_t key) noexcept {
        Set& s = sets_[set_idx(key)];
        PackedSlot v0 = load(&s.slot[0]);
        PackedSlot v1 = load(&s.slot[1]);
        PackedSlot v2 = load(&s.slot[2]);
        PackedSlot v3 = load(&s.slot[3]);

        uint64_t m0 = 0 - static_cast<uint64_t>(unpack_key(v0) == key);
        uint64_t m1 = 0 - static_cast<uint64_t>(unpack_key(v1) == key);
        uint64_t m2 = 0 - static_cast<uint64_t>(unpack_key(v2) == key);
        uint64_t m3 = 0 - static_cast<uint64_t>(unpack_key(v3) == key);

        uint64_t any = m0 | m1 | m2 | m3;
        if (!any)
            return;

        unsigned idx = static_cast<unsigned>(__builtin_ctzll(any));
        PackedSlot vs[4] = {v0, v1, v2, v3};
        PackedSlot empty{};
        if (cas_relaxed(&s.slot[idx], &vs[idx], empty)) {
            if (on_evict_)
                on_evict_(unpack_val(vs[idx]));
        }
    }

    // Clear all entries. Calls on_evict for each occupied slot.
    // Thread-safe: each slot is CAS'd to empty before firing callback.
    void clear() noexcept {
        PackedSlot empty{};
        for (size_t s = 0; s < kSets; ++s) {
            for (size_t i = 0; i < kAssoc; ++i) {
                for (;;) {
                    PackedSlot old = load(&sets_[s].slot[i]);
                    if (unpack_key(old) == 0)
                        break;
                    if (cas_relaxed(&sets_[s].slot[i], &old, empty)) {
                        if (on_evict_)
                            on_evict_(unpack_val(old));
                        break;
                    }
                    // CAS failed (concurrent modification) — retry.
                }
            }
        }
    }

private:
    EvictFn on_evict_ = nullptr;
    alignas(kCL) Set sets_[kSets]{};
    uint8_t hands_[kSets]{};  // per-set CLOCK hand (relaxed atomic from put())
};
