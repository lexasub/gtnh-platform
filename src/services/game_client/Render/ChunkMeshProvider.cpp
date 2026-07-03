#include "ChunkMeshProvider.h"
#include "../Cache/ChunkMeshCache.h"
#include "../World/World.h"
#include <glm/glm.hpp>

#include "RenderLib/Scene/Frustum.h"

ChunkMeshProvider::ChunkMeshProvider(World* world, ChunkMeshCache* meshCache)
    : world_(world), meshCache_(meshCache) {}

void ChunkMeshProvider::ForEachVisibleMesh(const renderlib::Frustum& frustum,
                                           std::function<void(const renderlib::MeshDrawData&)> callback) const {
    if (!world_ || !meshCache_) return;
    meshCache_->ForEachChunk([&](const ChunkCoord& coord, const ChunkMesh& mesh) {
        // Фрустум-куллинг в мировых координатах (AABB чанка)
        glm::vec3 min(coord.x * CHUNK_SIZE, coord.y * CHUNK_SIZE, coord.z * CHUNK_SIZE);
        glm::vec3 max(min.x + CHUNK_SIZE, min.y + CHUNK_SIZE, min.z + CHUNK_SIZE);
        if (!frustum.IntersectsAABB(min, max))
            return;
        renderlib::MeshDrawData drawData;
        drawData.handles.vb.idx = mesh.vb.idx;
        drawData.handles.ib.idx = mesh.ib.idx;
        if (!mesh.cpuData.vertices.empty()) {
            drawData.cpuVertices = mesh.cpuData.vertices.data();
            drawData.numVertices = static_cast<uint32_t>(mesh.cpuData.vertices.size());
            drawData.cpuIndices = mesh.cpuData.indices.data();
            drawData.numIndices = static_cast<uint32_t>(mesh.cpuData.indices.size());
            drawData.vertexSize = sizeof(BlockVertex);
        } else {
            drawData.cpuVertices = nullptr;
        }
        callback(drawData);

        // Also yield transparent mesh if it exists
        if (bgfx::isValid(mesh.transparentVb)) {
            renderlib::MeshDrawData transparentDraw;
            transparentDraw.handles.vb = mesh.transparentVb;
            transparentDraw.handles.ib = mesh.transparentIb;
            // Don't set cpuVertices — use GPU path for transparent
            transparentDraw.transparent = true;
            callback(transparentDraw);
        }
    });
}