#include "CableMeshBuilder.h"
#include "CableColors.h"
#include "ChunkMeshBuilder.h"
#include "../Common/BlockVertex.h"
#include <cstdint>

namespace {

// cable geometry (thin — 28% of block)
constexpr float C_JUNC       = 0.36f;  // 0.36->0.64 = 28%
constexpr float C_FLANGE_IN  = 0.36f;
constexpr float C_FLANGE_OUT = 0.30f;  // 0.30->0.70 = 40%
constexpr float C_FLANGE_T   = 0.06f;

// ============================================================================
// Vertex helpers
// ============================================================================

inline void packNormal(float nx, float ny, float nz, uint8_t* out) {
    out[0] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
    out[1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
    out[2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f);
    out[3] = 0;
}

inline void addVert(ChunkMeshBuilder::MeshData& mesh,
                    float x, float y, float z,
                    float nx, float ny, float nz,
                    uint8_t r, uint8_t g, uint8_t b,
                    float u, float v) {
    BlockVertex bv;
    bv.x = x; bv.y = y; bv.z = z;
    packNormal(nx, ny, nz, bv.normal);
    bv.color[0] = r; bv.color[1] = g; bv.color[2] = b; bv.color[3] = 255;
    bv.u = u; bv.v = v;
    mesh.vertices.push_back(bv);
}

inline void addQuad(ChunkMeshBuilder::MeshData& mesh,
                    int v0, int v1, int v2, int v3) {
    mesh.indices.push_back(static_cast<uint16_t>(v0));
    mesh.indices.push_back(static_cast<uint16_t>(v1));
    mesh.indices.push_back(static_cast<uint16_t>(v2));
    mesh.indices.push_back(static_cast<uint16_t>(v0));
    mesh.indices.push_back(static_cast<uint16_t>(v2));
    mesh.indices.push_back(static_cast<uint16_t>(v3));
}

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

static constexpr int TUBE_SKIP_FACE[6] = {
    1, 0, 3, 2, 5, 4
};

struct Box { float x0, y0, z0, x1, y1, z1; };

void emitBoxFace(ChunkMeshBuilder::MeshData& mesh,
                 const Box& b, int face,
                 const uint8_t* color, int vertBase,
                 float u_off, float v_off,
                 float u_scale, float v_scale) {
    float ox = b.x0, oy = b.y0, oz = b.z0;
    float sx = b.x1 - b.x0, sy = b.y1 - b.y0, sz = b.z1 - b.z0;
    for (int v = 0; v < 4; ++v) {
        addVert(mesh,
                ox + FV[face][v][0] * sx,
                oy + FV[face][v][1] * sy,
                oz + FV[face][v][2] * sz,
                FN[face][0], FN[face][1], FN[face][2],
                color[0], color[1], color[2],
                u_off + FV[face][v][0] * u_scale,
                v_off + FV[face][v][1] * v_scale);
    }
    addQuad(mesh, vertBase, vertBase+1, vertBase+2, vertBase+3);
}

int emitBox(ChunkMeshBuilder::MeshData& mesh,
            const Box& b, const uint8_t* color) {
    int base = static_cast<int>(mesh.vertices.size());
    for (int f = 0; f < 6; ++f)
        emitBoxFace(mesh, b, f, color, base + f * 4, 0.0f, 0.0f, 1.0f, 1.0f);
    return base;
}

// 5-face tube box
void emitTube(ChunkMeshBuilder::MeshData& mesh,
              const Box& b, int face,
              const uint8_t* color, float v_len) {
    int skip = TUBE_SKIP_FACE[face];
    int vi = static_cast<int>(mesh.vertices.size());
    for (int f = 0; f < 6; ++f) {
        if (f == skip) continue;
        emitBoxFace(mesh, b, f, color, vi, 0.0f, 0.0f, 1.0f, (f == face) ? 1.0f : v_len);
        vi += 4;
    }
}

void emitFlange(ChunkMeshBuilder::MeshData& mesh,
                const Box& b, int face,
                const uint8_t* color) {
    emitTube(mesh, b, face, color, 1.0f);
}

void cableColor(uint8_t tier, uint8_t* out) {
    auto it = CABLE_COLORS.find(tier);
    if (it == CABLE_COLORS.end()) {
        out[0] = out[1] = out[2] = 128; out[3] = 255;
        return;
    }
    out[0] = static_cast<uint8_t>(it->second.r * 255.0f);
    out[1] = static_cast<uint8_t>(it->second.g * 255.0f);
    out[2] = static_cast<uint8_t>(it->second.b * 255.0f);
    out[3] = 255;
}

struct DirGeom { Box tube[6]; Box flange[6]; };

DirGeom makeDirGeom(float gx, float gy, float gz) {
    float fi = C_FLANGE_IN;
    float fo = C_FLANGE_OUT;
    float ft = C_FLANGE_T;
    float j  = C_JUNC;

    DirGeom dg;
    dg.tube[0]    = {gx+j, gy+fi, gz+fi, gx+1.0f, gy+1.0f-fi, gz+1.0f-fi};
    dg.flange[0]  = {gx+1.0f-ft, gy+fo, gz+fo, gx+1.0f, gy+1.0f-fo, gz+1.0f-fo};
    dg.tube[1]    = {gx+0.0f, gy+fi, gz+fi, gx+1.0f-j, gy+1.0f-fi, gz+1.0f-fi};
    dg.flange[1]  = {gx+0.0f, gy+fo, gz+fo, gx+ft, gy+1.0f-fo, gz+1.0f-fo};
    dg.tube[2]    = {gx+fi, gy+j, gz+fi, gx+1.0f-fi, gy+1.0f, gz+1.0f-fi};
    dg.flange[2]  = {gx+fo, gy+1.0f-ft, gz+fo, gx+1.0f-fo, gy+1.0f, gz+1.0f-fo};
    dg.tube[3]    = {gx+fi, gy+0.0f, gz+fi, gx+1.0f-fi, gy+1.0f-j, gz+1.0f-fi};
    dg.flange[3]  = {gx+fo, gy+0.0f, gz+fo, gx+1.0f-fo, gy+ft, gz+1.0f-fo};
    dg.tube[4]    = {gx+fi, gy+fi, gz+j, gx+1.0f-fi, gy+1.0f-fi, gz+1.0f};
    dg.flange[4]  = {gx+fo, gy+fo, gz+1.0f-ft, gx+1.0f-fo, gy+1.0f-fo, gz+1.0f};
    dg.tube[5]    = {gx+fi, gy+fi, gz+0.0f, gx+1.0f-fi, gy+1.0f-fi, gz+1.0f-j};
    dg.flange[5]  = {gx+fo, gy+fo, gz+0.0f, gx+1.0f-fo, gy+1.0f-fo, gz+ft};
    return dg;
}

static constexpr FaceMask FACE_TO_MASK[6] = {
    FACE_EAST, FACE_WEST, FACE_UP, FACE_DOWN, FACE_SOUTH, FACE_NORTH,
};

} // anonymous namespace

FaceMask CableMeshBuilder::detectConnections(
    int32_t x, int32_t y, int32_t z, uint8_t tier,
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

ChunkMeshBuilder::MeshData CableMeshBuilder::buildCableMesh(
    int32_t x, int32_t y, int32_t z,
    uint8_t tier, FaceMask connections) {
    ChunkMeshBuilder::MeshData mesh;
    mesh.vertices.reserve(150);
    mesh.indices.reserve(300);

    uint8_t color[4];
    cableColor(tier, color);

    float gx = static_cast<float>(x);
    float gy = static_cast<float>(y);
    float gz = static_cast<float>(z);

    Box center = {gx+C_JUNC, gy+C_JUNC, gz+C_JUNC,
                  gx+1.0f-C_JUNC, gy+1.0f-C_JUNC, gz+1.0f-C_JUNC};
    emitBox(mesh, center, color);

    DirGeom dg = makeDirGeom(gx, gy, gz);
    float tubeLen = 1.0f - C_JUNC - C_JUNC;

    for (int f = 0; f < 6; ++f) {
        if (!(connections & FACE_TO_MASK[f])) continue;
        emitTube(mesh, dg.tube[f], f, color, tubeLen);
        emitFlange(mesh, dg.flange[f], f, color);
    }

    return mesh;
}
