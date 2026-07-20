#ifndef METRICS_H
#define METRICS_H

#include <atomic>
#include <cstdint>

struct Metrics {
    static Metrics& instance() {
        static Metrics m;
        return m;
    }

    std::atomic<uint64_t> air_count{0};
    std::atomic<uint64_t> bitpack4_count{0};
    std::atomic<uint64_t> flat_count{0};
    std::atomic<uint64_t> overflow_count{0};

    std::atomic<uint64_t> promote_bitpack4_to_flat{0};
    std::atomic<uint64_t> promote_flat_to_overflow{0};
    std::atomic<uint64_t> demote_overflow_to_flat{0};
    std::atomic<uint64_t> demote_to_air{0};

    std::atomic<uint64_t> encode_us_total{0};
    std::atomic<uint64_t> encode_count{0};
    std::atomic<uint64_t> decode_us_total{0};
    std::atomic<uint64_t> decode_count{0};

    void dump() const;
};

#endif //METRICS_H
