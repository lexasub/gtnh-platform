#pragma once

#include <cstdint>
#include <memory>
#include <tbb/concurrent_hash_map.h>
#include "Types.h"

class ChunkView; // forward

// Concurrent storage of chunks (owned by World)
class ChunkStorage {
public:
    ChunkStorage();
    ~ChunkStorage();

    // Accessors
    bool HasChunk(ChunkCoord c) const;
    std::shared_ptr<const ChunkView> GetChunk(ChunkCoord c) const;

    // Mutation (Concurrent)
    void RemoveChunk(ChunkCoord c);
    std::shared_ptr<const ChunkView> StoreAndGetChunk(const ChunkCoord &c, std::shared_ptr<ChunkView> chunk);
    void Clear();

    size_t Size() const;

    template<typename F>
    void ForEachCoord(F&& fn) const {
        for (auto& entry : chunks_.range()) {
            fn(ChunkKeyToCoord(entry.first), entry.second);
        }
    }


private:
    struct Uint64Hash {
        static size_t hash(uint64_t key) {
            key = (~key) + (key << 21);
            key = key ^ (key >> 24);
            key = (key + (key << 3)) + (key << 8);
            key = key ^ (key >> 14);
            key = (key + (key << 2)) + (key << 4);
            key = key ^ (key >> 28);
            key = key + (key << 31);
            return static_cast<size_t>(key);
        }
        static bool equal(uint64_t a, uint64_t b) {
            return a == b;
        }
    };
    using ChunkMap = tbb::concurrent_hash_map<uint64_t, std::shared_ptr<ChunkView>, Uint64Hash>;
    ChunkMap chunks_;
};