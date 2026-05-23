#include "CableMeshBuilder.h"
#include "CableColors.h"
#include "ChunkMeshBuilder.h"
#include "../Common/BlockVertex.h"
#include <glm/glm.hpp>

namespace {
    inline void packNormal(float nx, float ny, float nz, uint8_t* out) {
        out[0] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
        out[1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
        out[2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f);
        out[3] = 0;
    }

    inline void addVert(ChunkMeshBuilder::MeshData& mesh,
                        float x, float y, float z,
                        float nx, float ny, float nz,
                        uint8_t r, uint8_t g, uint8_t b) {
        BlockVertex v;
        v.x = x; v.y = y; v.z = z;
        packNormal(nx, ny, nz, v.normal);
        v.color[0] = r; v.color[1] = g; v.color[2] = b; v.color[3] = 255;
        v.u = 0.0f; v.v = 0.0f;
        mesh.vertices.push_back(v);
    }

    inline void addQuad(ChunkMeshBuilder::MeshData& mesh, int v0, int v1, int v2, int v3) {
        mesh.indices.push_back(v0);
        mesh.indices.push_back(v1);
        mesh.indices.push_back(v2);
        mesh.indices.push_back(v0);
        mesh.indices.push_back(v2);
        mesh.indices.push_back(v3);
    }

    void emitBoxFace(ChunkMeshBuilder::MeshData& mesh,
                     float ox, float oy, float oz,
                     float sx, float sy, float sz,
                     int face, const uint8_t* color, int vertBase) {
        static constexpr float FV[6][4][3] = {
            {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
            {{0,0,1},{0,1,1},{0,1,0},{0,0,0}},
            {{0,1,1},{1,1,1},{1,1,0},{0,1,0}},
            {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
            {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
            {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},
        };
        static constexpr float FN[6][3] = {
            {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
        };
        for (int v = 0; v < 4; ++v) {
            addVert(mesh,
                    ox + FV[face][v][0] * sx,
                    oy + FV[face][v][1] * sy,
                    oz + FV[face][v][2] * sz,
                    FN[face][0], FN[face][1], FN[face][2],
                    color[0], color[1], color[2]);
        }
        addQuad(mesh, vertBase, vertBase+1, vertBase+2, vertBase+3);
    }

    int emitBox(ChunkMeshBuilder::MeshData& mesh,
                float ox, float oy, float oz,
                float sx, float sy, float sz,
                const uint8_t* color) {
        int base = static_cast<int>(mesh.vertices.size());
        for (int f = 0; f < 6; ++f)
            emitBoxFace(mesh, ox, oy, oz, sx, sy, sz, f, color, base + f * 4);
        return base;
    }
}

FaceMask CableMeshBuilder::detectConnections(int32_t x, int32_t y, int32_t z, uint8_t tier,
                               std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock) {
    FaceMask connections = 0;
    uint16_t id = static_cast<uint16_t>(79 + tier);
    if (getBlock(x,   y+1, z  ) == id) connections |= FACE_UP;
    if (getBlock(x,   y-1, z  ) == id) connections |= FACE_DOWN;
    if (getBlock(x,   y,   z-1) == id) connections |= FACE_NORTH;
    if (getBlock(x,   y,   z+1) == id) connections |= FACE_SOUTH;
    if (getBlock(x-1, y,   z  ) == id) connections |= FACE_WEST;
    if (getBlock(x+1, y,   z  ) == id) connections |= FACE_EAST;
    return connections;
}

ChunkMeshBuilder::MeshData CableMeshBuilder::buildCableMesh(int32_t x, int32_t y, int32_t z, uint8_t tier, FaceMask connections) {
    ChunkMeshBuilder::MeshData mesh;
    mesh.vertices.reserve(100);
    mesh.indices.reserve(150);

    auto it = CABLE_COLORS.find(tier);
    if (it == CABLE_COLORS.end()) return mesh;
    uint8_t color[4] = {
        static_cast<uint8_t>(it->second.r * 255.0f),
        static_cast<uint8_t>(it->second.g * 255.0f),
        static_cast<uint8_t>(it->second.b * 255.0f),
        255
    };

    float gx = static_cast<float>(x);
    float gy = static_cast<float>(y);
    float gz = static_cast<float>(z);

    emitBox(mesh, gx+0.35f, gy+0.35f, gz+0.35f, 0.3f, 0.3f, 0.3f, color);

    int vBase = static_cast<int>(mesh.vertices.size());

    struct { FaceMask face; float x0,y0,z0,x1,y1,z1; } bars[6] = {
        {FACE_UP,    0.45f, 0.65f, 0.45f,  0.55f, 1.00f, 0.55f},
        {FACE_DOWN,  0.45f, 0.00f, 0.45f,  0.55f, 0.35f, 0.55f},
        {FACE_NORTH, 0.45f, 0.45f, 0.00f,  0.55f, 0.55f, 0.35f},
        {FACE_SOUTH, 0.45f, 0.45f, 0.65f,  0.55f, 0.55f, 1.00f},
        {FACE_WEST,  0.00f, 0.45f, 0.45f,  0.35f, 0.55f, 0.55f},
        {FACE_EAST,  0.65f, 0.45f, 0.45f,  1.00f, 0.55f, 0.55f},
    };
    static constexpr float BAR_NORMALS[6][3] = {
        {0,1,0},{0,-1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},
    };

    for (auto& b : bars) {
        if (!(connections & b.face)) continue;
        int vb = vBase;
        addVert(mesh, gx+b.x0, gy+b.y0, gz+b.z0, BAR_NORMALS[b.face][0], BAR_NORMALS[b.face][1], BAR_NORMALS[b.face][2], color[0], color[1], color[2]);
        addVert(mesh, gx+b.x1, gy+b.y0, gz+b.z0, BAR_NORMALS[b.face][0], BAR_NORMALS[b.face][1], BAR_NORMALS[b.face][2], color[0], color[1], color[2]);
        addVert(mesh, gx+b.x1, gy+b.y1, gz+b.z1, BAR_NORMALS[b.face][0], BAR_NORMALS[b.face][1], BAR_NORMALS[b.face][2], color[0], color[1], color[2]);
        addVert(mesh, gx+b.x0, gy+b.y1, gz+b.z1, BAR_NORMALS[b.face][0], BAR_NORMALS[b.face][1], BAR_NORMALS[b.face][2], color[0], color[1], color[2]);
        addQuad(mesh, vb, vb+1, vb+2, vb+3);
        vBase += 4;
    }

    return mesh;
}
