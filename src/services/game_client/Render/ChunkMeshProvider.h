#pragma once

#include "../RenderLib/Common/IMeshProvider.h"
#include "../Cache/ChunkMeshCache.h" // игровой класс
#include "../World/World.h"

class ChunkMeshProvider : public renderlib::IMeshProvider {
public:
    ChunkMeshProvider(World* world, ChunkMeshCache* meshCache);
    void ForEachVisibleMesh(const renderlib::Frustum& frustum,
                            std::function<void(const renderlib::MeshDrawData&)> callback) const override;
private:
    World* world_;
    ChunkMeshCache* meshCache_;
};