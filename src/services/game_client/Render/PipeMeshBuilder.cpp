#include "PipeMeshBuilder.h"
#include "ChunkMeshBuilder.h"
#include "../Common/BlockVertex.h"

namespace {
    inline void packNormal(float nx, float ny, float nz, uint8_t* out) {
        out[0] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
        out[1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
        out[2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f);
        out[3] = 0;
    }

    struct Box {
        float x0, y0, z0, x1, y1, z1;
    };

    void pipeColor(PipeType type, uint8_t* out) {
        switch (type) {
            case PipeType::ITEM_PIPE:        out[0]=0x80; out[1]=0x80; out[2]=0x80; out[3]=0xFF; break;
            case PipeType::DENSE_ITEM_PIPE:  out[0]=0xA0; out[1]=0xA0; out[2]=0xA0; out[3]=0xFF; break;
            case PipeType::FLUID_PIPE:       out[0]=0x40; out[1]=0x60; out[2]=0xAA; out[3]=0xFF; break;
            case PipeType::DENSE_FLUID_PIPE: out[0]=0x50; out[1]=0x70; out[2]=0xCC; out[3]=0xFF; break;
            default:                         out[0]=0x80; out[1]=0x80; out[2]=0x80; out[3]=0xFF; break;
        }
    }

    inline void addVert(ChunkMeshBuilder::MeshData& mesh, float x, float y, float z,
                        float nx, float ny, float nz,
                        const uint8_t* color) {
        BlockVertex v;
        v.x = x; v.y = y; v.z = z;
        packNormal(nx, ny, nz, v.normal);
        v.color[0] = color[0]; v.color[1] = color[1];
        v.color[2] = color[2]; v.color[3] = color[3];
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

    void emitBoxFace(ChunkMeshBuilder::MeshData& mesh, const Box& b, int face,
                     const uint8_t* color, int vertOffset) {
        static constexpr float faceVerts[6][4][3] = {
            {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
            {{0,0,1},{0,1,1},{0,1,0},{0,0,0}},
            {{0,1,1},{1,1,1},{1,1,0},{0,1,0}},
            {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
            {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
            {{1,0,0},{0,0,0},{0,1,0},{1,1,0}}
        };
        static constexpr float normals[6][3] = {
            {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
        };

        float ox = b.x0, oy = b.y0, oz = b.z0;
        float sx = b.x1 - b.x0, sy = b.y1 - b.y0, sz = b.z1 - b.z0;

        for (int v = 0; v < 4; ++v) {
            addVert(mesh,
                    ox + faceVerts[face][v][0] * sx,
                    oy + faceVerts[face][v][1] * sy,
                    oz + faceVerts[face][v][2] * sz,
                    normals[face][0], normals[face][1], normals[face][2],
                    color);
        }
        addQuad(mesh, vertOffset, vertOffset+1, vertOffset+2, vertOffset+3);
    }

    int emitBox(ChunkMeshBuilder::MeshData& mesh, const Box& b,
                const uint8_t* color) {
        int base = static_cast<int>(mesh.vertices.size());
        for (int f = 0; f < 6; ++f) {
            emitBoxFace(mesh, b, f, color, base + f * 4);
        }
        return base;
    }
}

FaceMask PipeMeshBuilder::detectConnections(int32_t x, int32_t y, int32_t z,
                                            PipeType type,
                                            std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock) {
    FaceMask mask = 0;
    uint16_t target = pipeTypeToBlockId(type);

    if (getBlock(x,   y+1, z  ) == target) mask |= FACE_UP;
    if (getBlock(x,   y-1, z  ) == target) mask |= FACE_DOWN;
    if (getBlock(x,   y,   z-1) == target) mask |= FACE_NORTH;
    if (getBlock(x,   y,   z+1) == target) mask |= FACE_SOUTH;
    if (getBlock(x-1, y,   z  ) == target) mask |= FACE_WEST;
    if (getBlock(x+1, y,   z  ) == target) mask |= FACE_EAST;

    return mask;
}

ChunkMeshBuilder::MeshData PipeMeshBuilder::buildPipeMesh(int32_t x, int32_t y, int32_t z,
                                                          PipeType type, FaceMask connections) {
    ChunkMeshBuilder::MeshData mesh;
    mesh.vertices.reserve(120);
    mesh.indices.reserve(180);

    uint8_t color[4];
    pipeColor(type, color);

    float gx = static_cast<float>(x);
    float gy = static_cast<float>(y);
    float gz = static_cast<float>(z);

    Box center = {gx+0.2f, gy+0.2f, gz+0.2f, gx+0.8f, gy+0.8f, gz+0.8f};
    emitBox(mesh, center, color);

    struct Extrusion {
        FaceMask face;
        Box box;
    };
    Extrusion exts[6] = {
        {FACE_UP,    {gx+0.2f, gy+0.8f, gz+0.2f, gx+0.8f, gy+1.0f, gz+0.8f}},
        {FACE_DOWN,  {gx+0.2f, gy+0.0f, gz+0.2f, gx+0.8f, gy+0.2f, gz+0.8f}},
        {FACE_NORTH, {gx+0.2f, gy+0.2f, gz+0.0f, gx+0.8f, gy+0.8f, gz+0.2f}},
        {FACE_SOUTH, {gx+0.2f, gy+0.2f, gz+0.8f, gx+0.8f, gy+0.8f, gz+1.0f}},
        {FACE_WEST,  {gx+0.0f, gy+0.2f, gz+0.2f, gx+0.2f, gy+0.8f, gz+0.8f}},
        {FACE_EAST,  {gx+0.8f, gy+0.2f, gz+0.2f, gx+1.0f, gy+0.8f, gz+0.8f}},
    };

    for (auto& e : exts) {
        if (connections & e.face) {
            emitBox(mesh, e.box, color);
        }
    }

    return mesh;
}
