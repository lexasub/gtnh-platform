#include "ChunkMeshCache.h"
#include "../Render/ChunkMeshBuilder.h"
#include <spdlog/spdlog.h>
#include "../Common/Types.h"
#include <bgfx/bgfx.h>

ChunkMeshCache::~ChunkMeshCache() {
    Clear();
}

void ChunkMeshCache::MarkDirty(const ChunkCoord &coord) {
    MeshMap::accessor acc;
    if (meshes_.find(acc, MakeChunkKey(coord)))
        acc->second.dirty = true;
}

void ChunkMeshCache::DestroyMesh(const ChunkCoord &coord) {
    MeshMap::accessor acc;
    if (meshes_.find(acc, MakeChunkKey(coord))) {
        acc->second.DestroyGpuBuffers();
        meshes_.erase(acc);
    }
}

void ChunkMeshCache::EnqueueCreateMesh(const ChunkCoord& coord, uint64_t contentHash,
                                        ChunkMeshBuilder::MeshData&& data) {
    meshCreateQueue_.push({coord, contentHash, std::move(data)});
}

void ChunkMeshCache::EnqueueDestroyMesh(const ChunkCoord& coord) {
    meshDestroyQueue_.push({coord});
}

void ChunkMeshCache::ProcessPendingOps() {
    MeshCreateRequest createReq;
    while (meshCreateQueue_.try_pop(createReq)) {
        ApplyMeshData(std::move(createReq.data), createReq.coord, createReq.contentHash);
    }
    MeshDestroyRequest destroyReq;
    while (meshDestroyQueue_.try_pop(destroyReq)) {
        DestroyMesh(destroyReq.coord);
    }
}

void ChunkMeshCache::Clear() {
    for (auto& entry : meshes_.range())
        entry.second.DestroyGpuBuffers();
    meshes_.clear();
}

void ChunkMeshCache::DiscardHandles() {
    for (auto& entry : meshes_.range()) {
        entry.second.vb = BGFX_INVALID_HANDLE;
        entry.second.ib = BGFX_INVALID_HANDLE;
        entry.second.transparentVb = BGFX_INVALID_HANDLE;
        entry.second.transparentIb = BGFX_INVALID_HANDLE;
    }
    meshes_.clear();
}

size_t ChunkMeshCache::Size() const {
    return meshes_.size();
}

bool ChunkMeshCache::CheckBuildHash(const ChunkCoord& coord, uint64_t hash) const {
    MeshMap::const_accessor acc;
    if (!meshes_.find(acc, MakeChunkKey(coord)))
        return false;
    return acc->second.lastBuildHash == hash && acc->second.lastBuildHash != 0;
}

void ChunkMeshCache::ApplyMeshData(ChunkMeshBuilder::MeshData&& data, const ChunkCoord& coord, uint64_t contentHash) {
    if (data.vertices.empty() && data.transparentVertices.empty()) {
        // Destroy old GPU buffers even if new mesh is empty (chunk became all-air).
        DestroyMesh(coord);
        return;
    }

    uint64_t key = MakeChunkKey(coord);
    MeshMap::accessor acc;
    if (meshes_.find(acc, key)) {
        acc->second.DestroyGpuBuffers();
    } else {
        meshes_.insert(acc, key);
    }

    ChunkMesh& mesh = acc->second;
    mesh.cpuData = std::move(data);

    // Bake global coordinates into vertices (MeshData stores local coords 0..31)
    auto ox = static_cast<float>(coord.x * CHUNK_SIZE);
    auto oy = static_cast<float>(coord.y * CHUNK_SIZE);
    auto oz = static_cast<float>(coord.z * CHUNK_SIZE);
    for (auto& v : mesh.cpuData.vertices) {
        v.x += ox;
        v.y += oy;
        v.z += oz;
    }
    for (auto& v : mesh.cpuData.transparentVertices) {
        v.x += ox;
        v.y += oy;
        v.z += oz;
    }
    if (!mesh.cpuData.vertices.empty()) {
        mesh.cpuData.vertices.shrink_to_fit();
        mesh.vb = bgfx::createVertexBuffer(
            bgfx::copy(mesh.cpuData.vertices.data(),
                       static_cast<uint32_t>(mesh.cpuData.vertices.size() * sizeof(BlockVertex))),
            BlockVertexLayoutRef());
        mesh.cpuData.vertices.clear();
        mesh.cpuData.vertices.shrink_to_fit();

        mesh.cpuData.indices.shrink_to_fit();
        mesh.ib = bgfx::createIndexBuffer(
            bgfx::copy(mesh.cpuData.indices.data(),
                       static_cast<uint32_t>(mesh.cpuData.indices.size() * sizeof(uint16_t))));

        // Free CPU-side data — no longer needed after GPU upload.
        // Renderer::Render uses per-chunk submit (Phase 3) when cpuData is empty.
        mesh.cpuData.indices.clear();
        mesh.cpuData.indices.shrink_to_fit();
    }

    // Create GPU buffers for transparent faces if any
    if (!mesh.cpuData.transparentVertices.empty()) {
        mesh.cpuData.transparentVertices.shrink_to_fit();
        mesh.transparentVb = bgfx::createVertexBuffer(
            bgfx::copy(mesh.cpuData.transparentVertices.data(),
                       static_cast<uint32_t>(mesh.cpuData.transparentVertices.size() * sizeof(BlockVertex))),
            BlockVertexLayoutRef());
        mesh.cpuData.transparentVertices.clear();
        mesh.cpuData.transparentVertices.shrink_to_fit();

        mesh.cpuData.transparentIndices.shrink_to_fit();
        mesh.transparentIb = bgfx::createIndexBuffer(
            bgfx::copy(mesh.cpuData.transparentIndices.data(),
                       static_cast<uint32_t>(mesh.cpuData.transparentIndices.size() * sizeof(uint16_t))));

        // Free CPU-side data
        mesh.cpuData.transparentIndices.clear();
        mesh.cpuData.transparentIndices.shrink_to_fit();
    }

    mesh.dirty = false;
    mesh.lastBuildHash = contentHash;
}