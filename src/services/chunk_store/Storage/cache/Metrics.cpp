#include "Metrics.h"
#include <cstdio>

void Metrics::dump() const {
    printf("=== MutableSection Metrics ===\n");
    printf("  Promotions: BITPACK4→FLAT=%lu FLAT→OVERFLOW=%lu\n",
           promote_bitpack4_to_flat.load(), promote_flat_to_overflow.load());
    printf("  Demotions: OVERFLOW→FLAT=%lu →AIR=%lu\n",
           demote_overflow_to_flat.load(), demote_to_air.load());
    uint64_t ec = encode_count.load();
    uint64_t dc = decode_count.load();
    printf("  Encode: count=%lu avg=%.1f us\n", ec, ec ? (double)encode_us_total.load() / ec : 0.0);
    printf("  Decode: count=%lu avg=%.1f us\n", dc, dc ? (double)decode_us_total.load() / dc : 0.0);
}
