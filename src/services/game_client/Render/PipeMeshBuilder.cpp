#include "PipeMeshBuilder.h"
#include "ChunkMeshBuilder.h"
#include "../Common/BlockVertex.h"

namespace {

// pipe dimensions
constexpr float P_JUNC       = 0.28f;  // 0.28->0.72 = 44%
constexpr float P_FLANGE_IN  = 0.28f;
constexpr float P_FLANGE_OUT = 0.20f;  // 0.20->0.80 = 60%
constexpr float P_FLANGE_T   = 0.08f;

// cable dimensions (thinner)
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
                    const uint8_t* color,
                    float u, float v) {
    BlockVertex bv;
    bv.x = x; bv.y = y; bv.z = z;
    packNormal(nx, ny, nz, bv.normal);
    bv.color[0] = color[0]; bv.color[1] = color[1];
    bv.color[2] = color[2]; bv.color[3] = color[3];
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

// Face vertex table: FV[face][vert] = local {X,Y,Z} in [0,1]
// FN[face] = normal. Face indices: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
static constexpr float FV[6][4][3] = {
    {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},  // 0: +X
    {{0,0,1},{0,1,1},{0,1,0},{0,0,0}},  // 1: -X
    {{0,1,1},{1,1,1},{1,1,0},{0,1,0}},  // 2: +Y
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},  // 3: -Y
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},  // 4: +Z
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},  // 5: -Z
};

static constexpr float FN[6][3] = {
    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
};

// When emitting a tube in direction `face`, skip the face that connects
// to the center junction (avoids z-fighting with the junction box).
static constexpr int TUBE_SKIP_FACE[6] = {
    1, 0, 3, 2, 5, 4  // +X->-X, -X->+X, +Y->-Y, -Y->+Y, +Z->-Z, -Z->+Z
};

// ============================================================================
// Box / tube / flange emitters
// ============================================================================

struct Box { float x0, y0, z0, x1, y1, z1; };

// Single face quad. UV maps over the face local axes (0..1).
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
                color,
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

// 5-face tube box — skips the junction-connecting face.
// `face` = direction the tube extends (0..5).
// Side faces tile V along tube axis; end face gets flat UV.
void emitTube(ChunkMeshBuilder::MeshData& mesh,
              const Box& b, int face,
              const uint8_t* color, float v_len) {
    int skip = TUBE_SKIP_FACE[face];
    int vi = static_cast<int>(mesh.vertices.size());
    for (int f = 0; f < 6; ++f) {
        if (f == skip) continue;
        if (f == face)
            emitBoxFace(mesh, b, f, color, vi, 0.0f, 0.0f, 1.0f, 1.0f);
        else
            emitBoxFace(mesh, b, f, color, vi, 0.0f, 0.0f, 1.0f, v_len);
        vi += 4;
    }
}

void emitFlange(ChunkMeshBuilder::MeshData& mesh,
                const Box& b, int face,
                const uint8_t* color) {
    emitTube(mesh, b, face, color, 1.0f);
}

// ============================================================================
// Color selection
// ============================================================================

void pipeColor(PipeType type, uint8_t* out) {
    if (isCableType(type)) {
        const uint8_t* c = cableTierColor(pipeTypeToCableTier(type));
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[3];
        return;
    }
    switch (type) {
        case PipeType::ITEM_PIPE:        out[0]=0x80; out[1]=0x80; out[2]=0x80; out[3]=0xFF; break;
        case PipeType::DENSE_ITEM_PIPE:  out[0]=0xA0; out[1]=0xA0; out[2]=0xA0; out[3]=0xFF; break;
        case PipeType::FLUID_PIPE:       out[0]=0x40; out[1]=0x60; out[2]=0xAA; out[3]=0xFF; break;
        case PipeType::DENSE_FLUID_PIPE: out[0]=0x50; out[1]=0x70; out[2]=0xCC; out[3]=0xFF; break;
        default:                         out[0]=0x80; out[1]=0x80; out[2]=0x80; out[3]=0xFF; break;
    }
}

// ============================================================================

struct DirGeom { Box tube[6]; Box flange[6]; };

DirGeom makeDirGeom(float gx, float gy, float gz, bool isPipe) {
    float j  = isPipe ? P_JUNC : C_JUNC;
    float fi = isPipe ? P_FLANGE_IN  : C_FLANGE_IN;
    float fo = isPipe ? P_FLANGE_OUT : C_FLANGE_OUT;
    float ft = isPipe ? P_FLANGE_T   : C_FLANGE_T;

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

// ============================================================================
// Public API
// ============================================================================

FaceMask PipeMeshBuilder::detectConnections(
    int32_t x, int32_t y, int32_t z, PipeType type,
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

ChunkMeshBuilder::MeshData PipeMeshBuilder::buildPipeMesh(
    int32_t x, int32_t y, int32_t z,
    PipeType type, FaceMask connections) {
    ChunkMeshBuilder::MeshData mesh;
    bool isCable = isCableType(type);

    mesh.vertices.reserve(isCable ? 150 : 300);
    mesh.indices.reserve(isCable ? 300 : 600);

    uint8_t color[4];
    pipeColor(type, color);

    float gx = static_cast<float>(x);
    float gy = static_cast<float>(y);
    float gz = static_cast<float>(z);

    float jh = isCable ? C_JUNC : P_JUNC;
    Box center = {gx+jh, gy+jh, gz+jh, gx+1.0f-jh, gy+1.0f-jh, gz+1.0f-jh};
    emitBox(mesh, center, color);

    DirGeom dg = makeDirGeom(gx, gy, gz, !isCable);
    float tubeLen = 1.0f - jh - jh;

    for (int f = 0; f < 6; ++f) {
        if (!(connections & FACE_TO_MASK[f])) continue;
        emitTube(mesh, dg.tube[f], f, color, tubeLen);
        emitFlange(mesh, dg.flange[f], f, color);
    }

    return mesh;
}
