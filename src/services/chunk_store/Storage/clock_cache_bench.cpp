// ClockCache vs LruCache microbenchmark.
//
// Build:
//   cmake .. -DCMAKE_BUILD_TYPE=Release && make clock_cache_bench -j
//
// Quick perf run:
//   perf stat ./clock_cache_bench
//
// Full perf counters:
//   perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses
//     ./clock_cache_bench

#include "cache/ClockCache.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <list>
#include <optional>
#include <vector>
#include <thread>
#include <cstring>

// ============================================================================
// LruCache — reconstructed from the old LruCache.h for A/B comparison.
// Stores uintptr_t (same as ClockCache), not shared_ptr — fair fight.
// ============================================================================
template<typename Key, typename Value>
class LruCache {
public:
    explicit LruCache(size_t max_size) : max_size_(max_size) {}

    void put(const Key& key, Value value) {
        std::unique_lock lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = std::move(value);
            lru_.splice(lru_.begin(), lru_, it->second);
        } else {
            if (lru_.size() >= max_size_) {
                auto last = lru_.back();
                map_.erase(last.first);
                lru_.pop_back();
            }
            lru_.emplace_front(key, std::move(value));
            map_[key] = lru_.begin();
        }
    }

    std::optional<Value> get(const Key& key) {
        std::unique_lock lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->second;
    }

    void erase(const Key& key) {
        std::unique_lock lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            lru_.erase(it->second);
            map_.erase(it);
        }
    }

    void clear() {
        std::unique_lock lock(mutex_);
        lru_.clear();
        map_.clear();
    }

private:
    size_t max_size_;
    std::list<std::pair<Key, Value>> lru_;
    std::unordered_map<Key, typename decltype(lru_)::iterator> map_;
    mutable std::shared_mutex mutex_;
};

// ============================================================================
// Benchmark harness
// ============================================================================
static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct BenchResult {
    const char* name;
    uint64_t elapsed_ns;
    uint64_t ops;
    double   ns_per_op;   // lower is better
    double   ops_per_sec; // higher is better
};

static std::vector<BenchResult> g_results;

static void report(const BenchResult& r) {
    printf("  %-40s  %8.1f ns/op  %10.2f M ops/sec\n",
           r.name, r.ns_per_op, r.ops_per_sec / 1e6);
}

static void bench_put_sequential(const char* label, auto& cache, int64_t count) {
    uint64_t start = now_ns();
    for (int64_t i = 1; i <= count; ++i)
        cache.put(i, uintptr_t(1000 + i));
    uint64_t end = now_ns();

    BenchResult r{label, end - start, static_cast<uint64_t>(count),
                  double(end - start) / count,
                  double(count) / (double(end - start) / 1e9)};
    report(r);
    g_results.push_back(r);
}

static void bench_get_random(const char* label, auto& cache, const int64_t* keys,
                             int64_t count) {
    uint64_t start = now_ns();
    for (int64_t i = 0; i < count; ++i) {
        volatile auto v = cache.get(keys[i]);
        (void)v;
    }
    uint64_t end = now_ns();

    BenchResult r{label, end - start, static_cast<uint64_t>(count),
                  double(end - start) / count,
                  double(count) / (double(end - start) / 1e9)};
    report(r);
    g_results.push_back(r);
}

static void bench_mixed(const char* label, auto& cache, const int64_t* keys,
                        int64_t count) {
    uint64_t start = now_ns();
    for (int64_t i = 0; i < count; ++i) {
        int64_t k = keys[i];
        switch (k & 3) {
        case 0: cache.put(k, uintptr_t(k));          break;
        case 1: { volatile auto v = cache.get(k); (void)v; } break;
        case 2: cache.erase(k);                      break;
        case 3: cache.put(k ^ 8192, uintptr_t(k));   break; // eviction pressure
        }
    }
    uint64_t end = now_ns();

    BenchResult r{label, end - start, static_cast<uint64_t>(count),
                  double(end - start) / count,
                  double(count) / (double(end - start) / 1e9)};
    report(r);
    g_results.push_back(r);
}

static void bench_concurrent(const char* label, auto& cache, int64_t ops_per_thread) {
    auto worker = [&](int seed) {
        unsigned s = static_cast<unsigned>(seed);
        for (int64_t i = 0; i < ops_per_thread; ++i) {
            int64_t k = static_cast<int64_t>(rand_r(&s) & 16383) + 1;
            cache.put(k, uintptr_t(k));
            volatile auto v = cache.get(k);
            (void)v;
        }
    };

    uint64_t start = now_ns();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back(worker, t * 1000 + 42);
    for (auto& t : threads) t.join();
    uint64_t end = now_ns();
    int64_t total_ops = ops_per_thread * 4 * 2; // put + get per iteration

    BenchResult r{label, end - start, static_cast<uint64_t>(total_ops),
                  double(end - start) / total_ops,
                  double(total_ops) / (double(end - start) / 1e9)};
    report(r);
    g_results.push_back(r);
}

// ============================================================================
// Game-realistic benchmarks
// ============================================================================

// Chunk coordinate → uniform int64 key.
// ClockCache's set_idx does (key * golden_ratio) & (kSets-1).  We must ensure
// BOTH cx and cz influence the low 8 bits of (key * GR), otherwise every chunk
// row (same cz) maps to the same set → massive collisions.
//
// Simple + large offset avoids key 0 sentinel.  The multiplier is large enough
// that (cx+offset) * M blends cx's bits into the low byte even after * GR.
static int64_t chunk_key(int64_t cx, int64_t cz) noexcept {
    constexpr uint64_t M = 0x9E3779B97F4A7C15ULL;  // golden ratio
    uint64_t h = static_cast<uint64_t>(cx + 1000000) * M;
    h ^= static_cast<uint64_t>(cz + 1000000);
    return static_cast<int64_t>(h);
}

// Player walk: each step, access |radius|^2 chunks centered on position.
// With radius=4 (9×9=81 chunks/step) and step=1 chunk,
// ~73/81 (90%) of chunks overlap with the previous step.
struct WalkState {
    int64_t x, z;    // current chunk pos
    int     radius;  // view radius in chunks
    int     step_x, step_z; // direction
    int     phase;   // how many steps left in this direction (spiral)
};

static void walk_init(WalkState& s, int64_t start_x, int64_t start_z, int radius) {
    s.x = start_x;
    s.z = start_z;
    s.radius = radius;
    s.step_x = 0;
    s.step_z = -1;  // start going north
    s.phase = 0;
}

// Advance one step in a spiral (Ulam-style).
static void walk_step(WalkState& s) {
    // Spiral: north → east → south → west, growing radius every 2 turns
    // This is the standard "spiral out" walk.
    if (s.step_x == 0 && s.step_z == -1) {
        // just started or finished a north leg → go east
        s.step_x = 1;
        s.step_z = 0;
        s.phase++;
    } else if (s.step_x == 1 && s.step_z == 0) {
        // went east → go south
        s.step_x = 0;
        s.step_z = 1;
    } else if (s.step_x == 0 && s.step_z == 1) {
        // went south → go west
        s.step_x = -1;
        s.step_z = 0;
        s.phase++;
    } else {
        // went west → go north
        s.step_x = 0;
        s.step_z = -1;
    }
    // Every pair of legs increases step count by 1 (same as "more legs, longer each time")
    int steps = 1 + s.phase / 2;
    s.x += s.step_x * steps;
    s.z += s.step_z * steps;
}

// Player exploring: spiral walk through new territory (cold cache).
// On cache miss, loads the chunk into cache (simulates LMDB read).
// Returns (total, hits).
static std::pair<uint64_t, uint64_t> bench_player_explore(auto& cache, int64_t start_x,
                                                           int64_t start_z, int view_radius,
                                                           int walk_steps) {
    WalkState ws;
    walk_init(ws, start_x, start_z, view_radius);
    uint64_t total = 0, hits = 0;
    for (int step = 0; step < walk_steps; ++step) {
        for (int dx = -ws.radius; dx <= ws.radius; ++dx) {
            for (int dz = -ws.radius; dz <= ws.radius; ++dz) {
                int64_t k = chunk_key(ws.x + dx, ws.z + dz);
                auto v = cache.get(k);
                if (v.has_value())
                    hits++;
                else
                    cache.put(k, uintptr_t(k));  // load from LMDB on miss
                total++;
            }
        }
        walk_step(ws);
    }
    return {total, hits};
}

// Player in loaded base: random walk bounded within a pre-loaded area.
// On cache miss, loads the chunk (cache was pre-populated; misses happen
// when builders evicted the chunk or player entered unloaded area).
// Returns (total, hits).
static std::pair<uint64_t, uint64_t> bench_player_hot(auto& cache, int64_t center_x,
                                                       int64_t center_z, int view_radius,
                                                       int walk_steps, int world_radius) {
    unsigned seed = static_cast<unsigned>(center_x ^ (center_z << 8));
    // Confine walk so the view never leaves [center ± world_radius]
    int margin = world_radius - view_radius;
    if (margin < 1) margin = 1;
    int64_t cx = center_x, cz = center_z;
    uint64_t total = 0, hits = 0;
    for (int step = 0; step < walk_steps; ++step) {
        for (int dx = -view_radius; dx <= view_radius; ++dx) {
            for (int dz = -view_radius; dz <= view_radius; ++dz) {
                int64_t k = chunk_key(cx + dx, cz + dz);
                auto v = cache.get(k);
                if (v.has_value())
                    hits++;
                else
                    cache.put(k, uintptr_t(k));  // load on miss
                total++;
            }
        }
        // Random step within bounds
        int64_t nx = cx + static_cast<int64_t>(rand_r(&seed) % 3) - 1;
        int64_t nz = cz + static_cast<int64_t>(rand_r(&seed) % 3) - 1;
        if (nx >= center_x - margin && nx <= center_x + margin) cx = nx;
        if (nz >= center_z - margin && nz <= center_z + margin) cz = nz;
    }
    return {total, hits};
}

// Miner thread: put blocks within a bounded pre-loaded area.
static std::pair<uint64_t, uint64_t> bench_miner_hot(auto& cache, int64_t center_x,
                                                      int64_t center_z, int ops,
                                                      int world_radius) {
    unsigned seed = static_cast<unsigned>(center_x ^ (center_z << 4));
    uint64_t hits = 0;
    for (int i = 0; i < ops; ++i) {
        int64_t k = chunk_key(center_x + static_cast<int64_t>(rand_r(&seed) % (world_radius * 2 + 1)) - world_radius, center_z + static_cast<int64_t>(rand_r(&seed) % (world_radius * 2 + 1)) - world_radius);
        auto v = cache.get(k);
        if (v.has_value()) hits++;
        cache.put(k, uintptr_t(k));
    }
    return {static_cast<uint64_t>(ops) * 2, hits};
}

struct BenchGameResult {
    const char* label;
    uint64_t total;
    uint64_t hits;
    double   hit_rate_pct;
    uint64_t elapsed_ns;
    double   ns_per_op;
};

static std::vector<BenchGameResult> g_game_results;

static void report_game(const BenchGameResult& r) {
    printf("  %-50s %7.1f ns/op  %8.2f M/s  hit %5.1f%%\n",
           r.label, r.ns_per_op,
           double(r.total) / (double(r.elapsed_ns) / 1e9) / 1e6,
           r.hit_rate_pct);
}

// Scenario: players exploring new territory (bench_player_explore, spiral walk).
static void bench_cold_scenario(const char* label,
                                 auto& cache,
                                 int n_players,
                                 int n_miners,
                                 int view_radius,
                                 int walk_steps,
                                 int miner_ops) {
    uint64_t start = now_ns();
    std::vector<std::thread> threads;
    std::vector<std::pair<uint64_t, uint64_t>> results(n_players + n_miners);

    for (int i = 0; i < n_players; ++i) {
        threads.emplace_back([&, i]() {
            results[i] = bench_player_explore(cache, i * 37, i * 53, view_radius, walk_steps);
        });
    }

    for (int i = 0; i < n_miners; ++i) {
        threads.emplace_back([&, i]() {
            results[n_players + i] = bench_miner_hot(cache,
                i * 1000 + 500, i * 1000 + 500, miner_ops, 128);
        });
    }

    for (auto& t : threads) t.join();
    uint64_t end = now_ns();

    uint64_t total = 0, hits = 0;
    for (auto& r : results) {
        total += r.first;
        hits  += r.second;
    }

    BenchGameResult r{label, total, hits,
                      double(hits) / double(total) * 100.0,
                      end - start,
                      double(end - start) / double(total)};
    report_game(r);
    g_game_results.push_back(r);
}

// Scenario: players in a pre-loaded base area (bench_player_hot, random walk).
// world_radius controls the pre-populated area; all activity stays within it.
static void bench_hot_scenario(const char* label,
                                auto& cache,
                                int n_players,
                                int n_miners,
                                int view_radius,
                                int walk_steps,
                                int miner_ops,
                                int world_radius) {
    // Pre-populate the world within [±world_radius]
    for (int64_t cx = -world_radius; cx <= world_radius; ++cx)
        for (int64_t cz = -world_radius; cz <= world_radius; ++cz)
            cache.put(chunk_key(cx, cz), uintptr_t(chunk_key(cx, cz)));

    uint64_t start = now_ns();
    std::vector<std::thread> threads;
    std::vector<std::pair<uint64_t, uint64_t>> results(n_players + n_miners);

    for (int i = 0; i < n_players; ++i) {
        threads.emplace_back([&, i]() {
            results[i] = bench_player_hot(cache,
                static_cast<int64_t>(i) * 10 - world_radius/2,
                static_cast<int64_t>(i) * 10 - world_radius/2,
                view_radius, walk_steps, world_radius);
        });
    }

    for (int i = 0; i < n_miners; ++i) {
        threads.emplace_back([&, i]() {
            results[n_players + i] = bench_miner_hot(cache, 0, 0, miner_ops, world_radius);
        });
    }

    for (auto& t : threads) t.join();
    uint64_t end = now_ns();

    uint64_t total = 0, hits = 0;
    for (auto& r : results) {
        total += r.first;
        hits  += r.second;
    }

    BenchGameResult r{label, total, hits,
                      double(hits) / double(total) * 100.0,
                      end - start,
                      double(end - start) / double(total)};
    report_game(r);
    g_game_results.push_back(r);
}

// ============================================================================
// main
// ============================================================================
int main() {
    constexpr int64_t kCount = 500'000;
    constexpr size_t  kCapacity = 1024;

    printf("=== ClockCache vs LruCache benchmark ===\n");
    printf("  Capacity: %zu, Ops per test: %ld\n\n", kCapacity, kCount);

    // Pre-generate random key sequence (same for both caches)
    std::vector<int64_t> keys(kCount);
    unsigned seed = 42;
    for (auto& k : keys)
        k = static_cast<int64_t>(rand_r(&seed) & 16383) + 1;

    // ---- Warmup (ClockCache) ----
    {
        ClockCache<uintptr_t, kCapacity> warm;
        for (int64_t i = 1; i <= kCount / 10; ++i)
            warm.put(i, uintptr_t(1000 + i));
        for (int64_t i = 1; i <= kCount / 10; ++i)
            warm.get(i);
        printf("Warmup done.\n\n");
    }

    // ---- ClockCache ----
    printf("--- ClockCache (lock-free, 4-way set-assoc) ---\n");
    {
        ClockCache<uintptr_t, kCapacity> cc;

        bench_put_sequential("put (all new keys)", cc, kCount);

        // Repopulate for get benchmark
        for (int64_t i = 1; i <= kCount; ++i)
            cc.put(keys[i % kCount], uintptr_t(keys[i % kCount]));

        bench_get_random("get (random, 80%+ hit rate)", cc, keys.data(), kCount);
        bench_mixed("mixed 25% put/25% get/25% erase/25% evict", cc, keys.data(), kCount);
        bench_concurrent("concurrent (4 threads put+get)", cc, kCount / 4);
    }

    printf("\n");

    // ---- LruCache ----
    printf("--- LruCache (mutex-guarded, std::list + hashmap) ---\n");
    {
        LruCache<int64_t, uintptr_t> lc(kCapacity);

        bench_put_sequential("put (all new keys)", lc, kCount);

        for (int64_t i = 1; i <= kCount; ++i)
            lc.put(keys[i % kCount], uintptr_t(keys[i % kCount]));

        bench_get_random("get (random, 80%+ hit rate)", lc, keys.data(), kCount);
        bench_mixed("mixed 25% put/25% get/25% erase/25% evict", lc, keys.data(), kCount);
        bench_concurrent("concurrent (4 threads put+get)", lc, kCount / 4);
    }

    printf("\n");

    // ---- Cold exploration (10 players + 2 miners) ----
    printf("--- Cold exploration (10 players + 2 miners) ---\n");
    printf("  New territory, all cache misses. Measures get-on-miss throughput.\n\n");
    {
        printf("  ClockCache:\n");
        ClockCache<uintptr_t, kCapacity> cc;
        bench_cold_scenario("explore players=10 miners=2 view=4 steps=1000", cc,
                            10, 2, 4, 1000, 50000);

        printf("  LruCache:\n");
        LruCache<int64_t, uintptr_t> lc(kCapacity);
        bench_cold_scenario("explore players=10 miners=2 view=4 steps=1000", lc,
                            10, 2, 4, 1000, 50000);
    }

    printf("\n");

    // ---- Hot base (4 players, 2 builders) ----
    printf("--- Hot base (4 players + 2 builders, pre-loaded area) ---\n");
    printf("  World: 32x32 chunks = 1024 (exactly cache capacity).\n");
    printf("  Players: random walk within area, 5x5 view = 95%%+ hit rate.\n");
    printf("  Builders: put/get in same area (mining/placing).\n\n");
    {
        printf("  ClockCache:\n");
        ClockCache<uintptr_t, kCapacity> cc;
        bench_hot_scenario("hot players=4 builders=2 view=2 steps=500", cc,
                           4, 2, 2, 500, 10000, 16);

        printf("  LruCache:\n");
        LruCache<int64_t, uintptr_t> lc(kCapacity);
        bench_hot_scenario("hot players=4 builders=2 view=2 steps=500", lc,
                           4, 2, 2, 500, 10000, 16);
    }

    printf("\n");

    // ---- Smoke test: put N keys, get the same N keys ----
    printf("--- Smoke test (put 800 keys, get same 800 keys) ---\n");
    printf("  All 800 keys should fit in 1024-slot cache without eviction.\n\n");
    {
        printf("  ClockCache:\n");
        {
            ClockCache<uintptr_t, kCapacity> cc;
            for (int64_t i = 1; i <= 800; ++i)
                cc.put(i, uintptr_t(i * 10));
            uint64_t hits = 0;
            for (int64_t i = 1; i <= 800; ++i) {
                auto v = cc.get(i);
                if (v.has_value()) hits++;
            }
            printf("  put 800 get 800: %lu hits out of 800 (%.1f%%)\n",
                   hits, double(hits) / 800.0 * 100.0);
        }
        printf("  LruCache:\n");
        {
            LruCache<int64_t, uintptr_t> lc(kCapacity);
            for (int64_t i = 1; i <= 800; ++i)
                lc.put(i, uintptr_t(i * 10));
            uint64_t hits = 0;
            for (int64_t i = 1; i <= 800; ++i) {
                auto v = lc.get(i);
                if (v.has_value()) hits++;
            }
            printf("  put 800 get 800: %lu hits out of 800 (%.1f%%)\n",
                   hits, double(hits) / 800.0 * 100.0);
        }

        printf("  ClockCache with chunk_key:\n");
        {
            ClockCache<uintptr_t, kCapacity> cc;
            for (int64_t cx = -14; cx <= 15; ++cx)
                for (int64_t cz = -14; cz <= 15; ++cz)
                    cc.put(chunk_key(cx, cz), uintptr_t(cx * 1000 + cz));
            uint64_t hits = 0, total = 0;
            for (int64_t cx = -14; cx <= 15; ++cx) {
                for (int64_t cz = -14; cz <= 15; ++cz) {
                    total++;
                    auto v = cc.get(chunk_key(cx, cz));
                    if (v.has_value()) hits++;
                }
            }
            printf("  put 900 chunk_keys, get same 900: %lu hits out of %lu (%.1f%%)\n",
                   hits, total, double(hits) / double(total) * 100.0);
        }
    }

    printf("\n");

    // ---- Sliding-window walk (1 player, near-100% hits) ----
    printf("--- Sliding-window walk (1 player, 11x11 view) ---\n");
    printf("  No pre-population. Player walks NE, 11x11 view.\n");
    printf("  First step: all misses (load into cache).\n");
    printf("  Steps 2-500: ~80%% overlap → ~80%% hit rate.\n\n");
    {
        constexpr int kView = 5;   // 11x11
        constexpr int kSteps = 500;

        auto walk_hit = [&](auto& cache, const char* label, int64_t start_x, int64_t start_z) {
            // Walk: at each step, get chunks in 11x11 view, load on miss.
            uint64_t total = 0, hits = 0;
            int64_t px = start_x, pz = start_z;
            uint64_t start = now_ns();
            for (int s = 0; s < kSteps; ++s) {
                for (int dx = -kView; dx <= kView; ++dx) {
                    for (int dz = -kView; dz <= kView; ++dz) {
                        int64_t k = chunk_key(px + dx, pz + dz);
                        auto v = cache.get(k);
                        if (v.has_value()) hits++;
                        else cache.put(k, uintptr_t(k));  // load on miss
                        total++;
                    }
                }
                px += 1; pz += 1;
            }
            uint64_t end = now_ns();
            BenchGameResult r{label, total, hits,
                              double(hits) / double(total) * 100.0,
                              end - start, double(end - start) / double(total)};
            report_game(r);
            g_game_results.push_back(r);
        };

        printf("  ClockCache:\n");
        {
            ClockCache<uintptr_t, kCapacity> cc;
            walk_hit(cc, "slide 1p view=11 steps=500", -10, -10);
        }

        printf("  LruCache:\n");
        {
            LruCache<int64_t, uintptr_t> lc(kCapacity);
            walk_hit(lc, "slide 1p view=11 steps=500", -10, -10);
        }
    }

    printf("\n");

    // ---- Summary ----
    printf("=== Summary ===\n");
    printf("  %-50s %12s  %12s   %s\n", "Benchmark", "ns/op", "M ops/sec", "Hit%");
    printf("  %-50s %12s  %12s   %s\n", "--------", "-----", "---------", "----");
    for (auto& r : g_results)
        printf("  %-50s %8.1f ns  %8.2f M/s\n", r.name, r.ns_per_op, r.ops_per_sec / 1e6);
    printf("\n");
    for (auto& r : g_game_results)
        printf("  %-50s %8.1f ns  %8.2f M/s  %5.1f%%\n", r.label, r.ns_per_op,
               double(r.total) / (double(r.elapsed_ns) / 1e9) / 1e6,
               r.hit_rate_pct);

    printf("\nFor perf counters, run:\n");
    printf("  perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses ./clock_cache_bench\n");
    printf("  perf stat -e L1-dcache-load-misses,LLC-load-misses ./clock_cache_bench\n");

    return 0;
}
