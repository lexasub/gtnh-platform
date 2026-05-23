#pragma once

#include <unordered_map>
#include <mutex>
#include <memory>
#include <tbb/concurrent_hash_map.h>
#include <oneapi/tbb/concurrent_queue.h>
#include "../Render/ChunkMeshBuilder.h"
#include "../Common/Types.h"

struct ChunkCoord;

struct ChunkMesh {
    uint64_t lastBuildHash = 0; // FNV-1a of chunk blocks; skip Build if unchanged
    bool dirty = true;

    // Persistent CPU-side vertex data. GPU buffers (vb/ib) are created from this
    // in ApplyMeshData(); Render() submits directly from the static GPU buffers.
    ChunkMeshBuilder::MeshData cpuData;

    bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;

    void DestroyGpuBuffers() {
        if (bgfx::isValid(vb)) bgfx::destroy(vb);
        if (bgfx::isValid(ib)) bgfx::destroy(ib);
        vb = BGFX_INVALID_HANDLE;
        ib = BGFX_INVALID_HANDLE;
    }
};

struct MeshCreateRequest {
    ChunkCoord coord{};
    uint64_t contentHash = 0;
    ChunkMeshBuilder::MeshData data;
};

struct MeshDestroyRequest {
    ChunkCoord coord{};
};

class ChunkMeshCache {
public:
    // CPU data is freed after GPU upload in ApplyMeshData(). Chunks without
    // cpuData are rendered via per-chunk GPU submit (Renderer Phase 3).

    ~ChunkMeshCache();

    void MarkDirty(const ChunkCoord &coord);
    void DestroyMesh(const ChunkCoord &coord);
    void Clear();
    void EnqueueCreateMesh(const ChunkCoord& coord, uint64_t contentHash,
                           ChunkMeshBuilder::MeshData&& data);
    void EnqueueDestroyMesh(const ChunkCoord& coord);
    void ProcessPendingOps();
    // Reset all GPU handles to invalid WITHOUT calling bgfx::destroy().
    // Use after bgfx::shutdown() on the render thread — the resources
    // are already freed, and calling destroy() on stale handles would
    // crash in bgfx::isValid().
    void DiscardHandles();
    size_t Size() const;



    // Thread-safe: returns true if chunk at coord already has a mesh for this hash.
    bool CheckBuildHash(const ChunkCoord& coord, uint64_t hash) const;

    // Called from any thread — takes ownership of MeshData for the chunk.
    // contentHash is stored so subsequent CheckBuildHash can skip rebuild.
    void ApplyMeshData(ChunkMeshBuilder::MeshData&& data, const ChunkCoord& coord, uint64_t contentHash = 0);

    template<typename F>
    void ForEachChunk(F&& visitor) {
        for (auto& entry : meshes_.range()) {
            ChunkCoord coord = ChunkKeyToCoord(entry.first);
            ChunkMesh& mesh = entry.second;
            visitor(coord, mesh);
        }
    }

private:
    struct Uint64Hash {
        static size_t hash(uint64_t key) {
            // Thomas Wang integer hash — spreads adjacent keys across TBB buckets
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
    using MeshMap = tbb::concurrent_hash_map<uint64_t, ChunkMesh, Uint64Hash>;
    MeshMap meshes_;

    tbb::concurrent_bounded_queue<MeshCreateRequest> meshCreateQueue_;
    tbb::concurrent_bounded_queue<MeshDestroyRequest> meshDestroyQueue_;
};