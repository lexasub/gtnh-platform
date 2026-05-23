// ChunkStore load tests — throughput, latency, mixed read/write, eviction stress.
//
// Build:
//   cmake .. -DCMAKE_BUILD_TYPE=Release && make chunkd_load_test -j
//
// Quick run:
//   ./chunkd_load_test
//
// All tests use temp LMDB databases (cleaned up on exit).

#include "ChunkStore.h"
#include "../Chunk/Chunk.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <algorithm>
#include <cmath>

// ============================================================================
// Timing helpers
// ============================================================================
using Clock = std::chrono::steady_clock;
using NS    = std::chrono::nanoseconds;

static uint64_t now_ns() {
    return std::chrono::duration_cast<NS>(Clock::now().time_since_epoch()).count();
}

struct Result {
    const char* name;
    uint64_t    ops;
    uint64_t    elapsed_ns;
    double      ns_per_op()  const { return double(elapsed_ns) / ops; }
    double      ops_per_sec() const { return double(ops) / (double(elapsed_ns) / 1e9); }
    double      mb_per_sec(double bytes_per_op) const {
        return bytes_per_op * ops_per_sec() / (1024.0 * 1024.0);
    }
};

static void print_result(const Result& r, double bytes_per_op = 0) {
    printf("  %-55s %9.0f ns/op  %9.2f M ops/s",
           r.name, r.ns_per_op(), r.ops_per_sec() / 1e6);
    if (bytes_per_op > 0)
        printf("  %6.1f MB/s", r.mb_per_sec(bytes_per_op));
    printf("\n");
}

// ============================================================================
// Temp DB helpers
// ============================================================================
static std::string make_temp_db() {
    char tmpl[] = "/tmp/chunkd_load_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

static void remove_db(const std::string& path) {
    if (!path.empty())
        std::filesystem::remove_all(path);
}

// ============================================================================
// Test 1: setBlock throughput — single thread, sequential chunks
// ============================================================================
static void test_setblock_seq() {
    printf("\n--- 1. setBlock sequential (1 thread) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);
        constexpr uint64_t kOps = 1000;

        uint64_t start = now_ns();
        for (uint64_t i = 0; i < kOps; ++i) {
            int32_t wx = static_cast<int32_t>((i % 800) * 32);
            int32_t wy = 0;
            int32_t wz = 0;
            store.setBlock(wx, wy, wz,
                           static_cast<uint16_t>(i & 0xFFFF),
                           static_cast<uint8_t>(i & 0xFF));
        }
        uint64_t end = now_ns();

        // Flush to LMDB (timed separately)
        uint64_t f_start = now_ns();
        store.flushDirtyChunks();
        uint64_t f_end = now_ns();

        print_result({"setBlock seq (cache only)", kOps, end - start});
        print_result({"flushDirtyChunks (to LMDB)", kOps, f_end - f_start});
        // Each chunk = ~260 bytes palette-encoded (observe skip 0x1000 for approx)
    }
    remove_db(db);
}

// ============================================================================
// Test 2: setBlock throughput — 4 threads, random chunks within cache capacity
// ============================================================================
static void test_setblock_concurrent() {
    printf("\n--- 2. SaveChunk concurrent (4 threads, disjoint ranges) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);
        constexpr uint64_t kOpsPerThread = 2000;
        constexpr int      kChunksPerThread = 200;

        auto worker = [&](int thread_id) {
            int base = thread_id * kChunksPerThread;
            for (uint64_t i = 0; i < kOpsPerThread; ++i) {
                int idx = base + (i % kChunksPerThread);
                // Touch chunk in cache
                store.getCached(idx, 0, 0);
                store.markDirty(idx, 0, 0);
            }
        };

        // Pre-populate cache with all chunks we'll touch
        for (int i = 0; i < 4 * kChunksPerThread; ++i) {
            auto* c = new Chunk();
            c->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, 0, 0, c);
        }

        uint64_t start = now_ns();
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back(worker, t);
        for (auto& t : threads) t.join();
        uint64_t end = now_ns();

        store.flushDirtyChunks();

        uint64_t total = kOpsPerThread * 4;
        print_result({"markDirty 4-thread disjoint", total, end - start});
    }
    remove_db(db);
}

// ============================================================================
// Test 3: Mixed read/write — 4 readers + 2 writers, random chunks
// ============================================================================
static void test_mixed_read_write() {
    printf("\n--- 3. Mixed read/write (4 readers + 2 writers, random chunks) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);
        constexpr int kChunks = 800;

        // Pre-populate cache
        for (int i = 0; i < kChunks; ++i) {
            auto* c = new Chunk();
            c->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, 0, 0, c);
        }

        std::atomic<bool> stop{false};

        // Flusher thread
        std::thread flusher([&]() {
            for (int i = 0; i < 50; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                store.flushDirtyChunks();
            }
        });

        std::vector<std::thread> threads;

        // 4 readers
        std::atomic<uint64_t> reads{0};
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                unsigned s = static_cast<unsigned>(t * 100);
                while (!stop.load(std::memory_order_relaxed)) {
                    int ci = rand_r(&s) % kChunks;
                    auto* c = store.getCached(ci, 0, 0);
                    if (c) { volatile auto v = c->blocks[0]; (void)v; }
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // 2 writers
        std::atomic<uint64_t> writes{0};
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                unsigned s = static_cast<unsigned>(t * 100 + 50);
                while (!stop.load(std::memory_order_relaxed)) {
                    int ci = rand_r(&s) % kChunks;
                    store.markDirty(ci, 0, 0);
                    writes.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Run for ~500ms
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop.store(true, std::memory_order_relaxed);

        for (auto& t : threads) t.join();
        flusher.join();

        store.flushDirtyChunks();

        uint64_t r = reads.load();
        uint64_t w = writes.load();
        printf("  %-55s %9lu reads  %9lu writes  total=%.0f K ops\n",
               "reads+writes", r, w, double(r + w) / 1000.0);
    }
    remove_db(db);
}

// ============================================================================
// Test 4: Flush latency — measure time between markDirty and LMDB persist
// ============================================================================
static void test_flush_latency() {
    printf("\n--- 4. Flush latency (mark → flush cycles) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);

        // Pre-populate 100 chunks in cache
        for (int i = 0; i < 100; ++i) {
            auto* c = new Chunk();
            c->blocks[0] = 42;
            store.putCached(i, 0, 0, c);
        }

        constexpr int kCycles = 1000;
        std::vector<uint64_t> latencies;
        latencies.reserve(kCycles);

        for (int cycle = 0; cycle < kCycles; ++cycle) {
            int ci = cycle % 100;
            store.markDirty(ci, 0, 0);

            uint64_t t0 = now_ns();
            store.flushDirtyChunks();
            uint64_t t1 = now_ns();

            latencies.push_back(t1 - t0);
        }

        // Stats
        std::sort(latencies.begin(), latencies.end());
        uint64_t total = 0;
        for (auto& l : latencies) total += l;
        double avg = double(total) / latencies.size();
        double med = latencies[latencies.size() / 2];
        double p99 = latencies[size_t(latencies.size() * 0.99)];
        double max = latencies.back();

        printf("  %-55s avg=%7.0f ns  med=%7.0f  p99=%7.0f  max=%7.0f\n",
               "flush latency (mark 1 chunk → LMDB)", avg, med, p99, max);
    }
    remove_db(db);
}

// ============================================================================
// Test 5: Flush latency with batch — mark N chunks, flush once
// ============================================================================
static void test_flush_batch_latency() {
    printf("\n--- 5. Flush batch latency (mark N chunks, flush once) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);

        // Pre-populate 500 chunks
        for (int i = 0; i < 500; ++i) {
            auto* c = new Chunk();
            c->blocks[0] = 42;
            store.putCached(i, 0, 0, c);
        }

        struct BatchResult { const char* label; int count; uint64_t ns; };

        auto bench_batch = [&](const char* label, int n_chunks) {
            // Mark N chunks as dirty
            for (int i = 0; i < n_chunks; ++i)
                store.markDirty(i, 0, 0);

            uint64_t t0 = now_ns();
            store.flushDirtyChunks();
            uint64_t t1 = now_ns();
            print_result({label, uint64_t(n_chunks), t1 - t0}, 260.0);
        };

        bench_batch("flush batch 1 chunk",  1);
        bench_batch("flush batch 10 chunks", 10);
        bench_batch("flush batch 100 chunks", 100);
        bench_batch("flush batch 500 chunks", 500);
    }
    remove_db(db);
}

// ============================================================================
// Test 6: Eviction stress — 2x cache capacity, concurrent reads+writes
// ============================================================================
static void test_eviction_stress() {
    printf("\n--- 6. Eviction stress (2000 chunks > 1024 cache, concurrent) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);
        constexpr int kChunks   = 2000;
        std::atomic<bool> stop{false};

        // Pre-populate all 2000 chunks
        for (int i = 0; i < kChunks; ++i) {
            auto* c = new Chunk();
            c->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, 0, 0, c);
        }

        // 4 writer threads — mark chunks as dirty (cycling through all 2000)
        std::vector<std::thread> writers;
        std::atomic<uint64_t> marks{0};
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&, t]() {
                unsigned s = static_cast<unsigned>(t * 100);
                while (!stop.load(std::memory_order_relaxed)) {
                    int ci = rand_r(&s) % kChunks;
                    store.markDirty(ci, 0, 0);
                    marks.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // 2 reader threads — getCached (many will miss due to eviction)
        std::vector<std::thread> readers;
        std::atomic<uint64_t> reads{0}, hits{0};
        for (int t = 0; t < 2; ++t) {
            readers.emplace_back([&, t]() {
                unsigned s = static_cast<unsigned>(t * 100 + 50);
                while (!stop.load(std::memory_order_relaxed)) {
                    int ci = rand_r(&s) % kChunks;
                    auto* c = store.getCached(ci, 0, 0);
                    if (c) hits.fetch_add(1, std::memory_order_relaxed);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Flusher thread
        std::thread flusher([&]() {
            for (int i = 0; i < 100; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                store.flushDirtyChunks();
            }
            stop.store(true, std::memory_order_relaxed);
        });

        for (auto& t : writers) t.join();
        for (auto& t : readers) t.join();
        flusher.join();

        store.flushDirtyChunks();

        uint64_t m = marks.load();
        uint64_t r = reads.load();
        uint64_t h = hits.load();
        double hit_rate = r > 0 ? double(h) / double(r) * 100.0 : 0;
        printf("  %-55s marks=%lu  reads=%lu  hit=%lu (%.1f%%)\n",
               "writers(4)+readers(2)+flusher", m, r, h, hit_rate);
        printf("  %-55s %.1f marks/s  %.1f reads/s\n",
               "aggregate throughput",
               double(m) / 0.5, double(r) / 0.5);
    }
    remove_db(db);
}

// ============================================================================
// Test 7: SaveChunk throughput — public API direct write to LMDB
// ============================================================================
static void test_savechunk_throughput() {
    printf("\n--- 7. SaveChunk throughput (10000 unique chunks, direct LMDB) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);

        constexpr int kChunks = 10'000;

        uint64_t start = now_ns();
        for (int i = 0; i < kChunks; ++i) {
            Chunk c;
            c.blocks[0] = static_cast<uint16_t>(i);
            store.SaveChunk(c, {i, 0, 0});
        }
        uint64_t end = now_ns();

        // Verify from second instance (cold cache forces LMDB read)
        {
            ChunkStore store2(db);
            uint64_t ok = 0;
            for (int i = 0; i < kChunks; ++i) {
                auto* c = store2.GetChunk({i, 0, 0});
                if (c && c->blocks[0] == static_cast<uint16_t>(i)) ++ok;
            }
            printf("  %-55s %lu/%d verified\n", "", ok, kChunks);
        }

        print_result({"SaveChunk x10k (unique keys, LMDB write)", uint64_t(kChunks), end - start},
                     sizeof(Chunk));
    }
    remove_db(db);
}

// ============================================================================
// Test 8: SaveChunk overwrite — same key, LMDB update throughput
// ============================================================================
static void test_savechunk_overwrite() {
    printf("\n--- 8. SaveChunk overwrite (10000 writes to same key) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);

        Chunk c;
        c.blocks[0] = 42;

        uint64_t start = now_ns();
        for (int i = 0; i < 10'000; ++i) {
            c.blocks[0] = static_cast<uint16_t>(i);
            store.SaveChunk(c, {42, 0, 0});
        }
        uint64_t end = now_ns();

        print_result({"SaveChunk x10k (same key, LMDB overwrite)", 10'000UL, end - start});
    }
    remove_db(db);
}

// ============================================================================
// Test 9: GetChunk bulk read — fresh instance = pure LMDB reads
// ============================================================================
static void test_getchunk_bulk_read() {
    printf("\n--- 9. GetChunk bulk read (10000 chunks from fresh instance, LMDB only) ---\n");
    auto db = make_temp_db();
    {
        // Write via one instance
        ChunkStore writer(db);
        constexpr int kChunks = 10'000;
        for (int i = 0; i < kChunks; ++i) {
            Chunk c;
            c.blocks[0] = static_cast<uint16_t>(i);
            writer.SaveChunk(c, {i, 0, 0});
        }
    }

    // Read from fresh instance (cold cache = pure LMDB reads)
    {
        ChunkStore reader(db);
        uint64_t start = now_ns();
        for (int i = 0; i < 10'000; ++i) {
            auto* c = reader.GetChunk({i, 0, 0});
            (void)c;
        }
        uint64_t end = now_ns();

        print_result({"GetChunk x10k (fresh LMDB, cold cache)", 10'000UL, end - start},
                     sizeof(Chunk));
    }
    remove_db(db);
}

// ============================================================================
// Test 10: Game-like scenario — spiral exploration + block placement
// ============================================================================
static void test_game_scenario() {
    printf("\n--- 10. Game-like scenario (spiral explore + block placement) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);

        // Phase 1: Place blocks along a spiral path (simulates player placing
        // torches / building while exploring)
        uint64_t total_placements = 0;

        uint64_t p_start = now_ns();
        // Spiral: 100 steps, place 5 blocks per step
        for (int step = 0; step < 100; ++step) {
            // Each step: player moves to (step*2, 0, step*2) and places 5 blocks
            int32_t bx = static_cast<int32_t>(step * 2);
            int32_t bz = static_cast<int32_t>(step * 2);
            for (int d = 0; d < 5; ++d) {
                store.setBlock(bx + d, 0, bz + d,
                               static_cast<uint16_t>(1 + d),
                               static_cast<uint8_t>(d));
                total_placements++;
            }
        }

        // Phase 2: Concurrent reading (simulating client view updates)
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> reads{0}, read_hits{0};

        std::thread reader([&]() {
            unsigned s = 42;
            while (!stop.load(std::memory_order_relaxed)) {
                int ci = rand_r(&s) % 100;
                auto* c = store.getCached(ci, 0, 0);
                if (c) read_hits.fetch_add(1, std::memory_order_relaxed);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });

        // Phase 3: Repeated flush + more placements (simulating build phase)
        for (int i = 0; i < 20; ++i) {
            store.flushDirtyChunks();
            for (int d = 0; d < 10; ++d) {
                store.setBlock(i * 10 + d, 5, i * 10 + d,
                               static_cast<uint16_t>(10 + d),
                               static_cast<uint8_t>(d));
                total_placements++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        store.flushDirtyChunks();
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        uint64_t p_end = now_ns();
        uint64_t r = reads.load();
        uint64_t h = read_hits.load();

        print_result({"block placements (explore+build)", total_placements, p_end - p_start});
        printf("  %-55s %lu reads  hit=%lu (%.1f%%)\n",
               "background reader (view update)", r, h,
               r > 0 ? double(h)/double(r)*100.0 : 0.0);
    }
    remove_db(db);
}

// ============================================================================
// Test 11: Long-running stability — 5s stress with metric sampling
// ============================================================================
static void test_long_running_stress() {
    printf("\n--- 11. Long-running stress (5s, high throughput) ---\n");
    auto db = make_temp_db();
    {
        ChunkStore store(db);
        constexpr int kChunks = 2000;
        std::atomic<bool> stop{false};

        // Pre-populate
        for (int i = 0; i < kChunks; ++i) {
            auto* c = new Chunk();
            c->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, 0, 0, c);
        }

        // 4 writers (setBlock via markDirty)
        std::atomic<uint64_t> writes{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&, t]() {
                unsigned s = static_cast<unsigned>(t * 100);
                while (!stop.load(std::memory_order_relaxed)) {
                    int ci = rand_r(&s) % kChunks;
                    store.markDirty(ci, 0, 0);
                    writes.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // 4 readers
        std::atomic<uint64_t> reads{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t) {
            readers.emplace_back([&, t]() {
                unsigned s = static_cast<unsigned>(t * 100 + 50);
                while (!stop.load(std::memory_order_relaxed)) {
                    int ci = rand_r(&s) % kChunks;
                    auto* c = store.getCached(ci, 0, 0);
                    if (c) { volatile auto v = c->blocks[0]; (void)v; }
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Flusher (every 10ms)
        std::thread flusher([&]() {
            for (int i = 0; i < 500; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                store.flushDirtyChunks();
                if (stop.load(std::memory_order_relaxed)) break;
            }
            stop.store(true, std::memory_order_relaxed);
        });

        std::this_thread::sleep_for(std::chrono::seconds(2));
        stop.store(true, std::memory_order_relaxed);

        for (auto& t : writers) t.join();
        for (auto& t : readers) t.join();
        flusher.join();

        store.flushDirtyChunks();

        uint64_t w = writes.load();
        uint64_t r = reads.load();
        printf("  %-55s %9lu writes (%.0f/s)  %9lu reads (%.0f/s)\n",
               "4 writers + 4 readers + flusher (5s)",
               w, double(w)/5.0, r, double(r)/5.0);
    }
    remove_db(db);
}

// ============================================================================
// main
// ============================================================================
int main() {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== ChunkStore Load Tests ===\n");
    printf("  Platform: Linux, C++26, ClockCache<1024>\n");
    printf("  Chunk size: %zu bytes raw, ~260 bytes palette-encoded\n\n", sizeof(Chunk));

    // Warmup — instantiate + open LMDB once
    {
        auto db = make_temp_db();
        ChunkStore warm(db);
        warm.setBlock(0, 0, 0, 1, 0);
        warm.flushDirtyChunks();
        remove_db(db);
        printf("  Warmup done.\n");
    }

    test_setblock_seq();
    test_setblock_concurrent();
    test_mixed_read_write();
    test_flush_latency();
    test_flush_batch_latency();
    test_eviction_stress();
    test_savechunk_throughput();
    test_savechunk_overwrite();
    test_getchunk_bulk_read();
    test_game_scenario();
    test_long_running_stress();

    printf("\n=== All load tests completed ===\n");
    return 0;
}
