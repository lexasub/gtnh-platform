#include "PipeMeshBuilder.h"
#include "ChunkMeshBuilder.h"
#include "BlockRenderRegistry.h"
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
                 const PipeMeshMaterial* material,
                 float v_len) {
    static constexpr int U_AXIS[6] = {2, 2, 0, 0, 0, 0};
    static constexpr int U_NEG[6] = {0, 1, 0, 0, 0, 1};
    static constexpr int V_AXIS[6] = {1, 1, 2, 2, 1, 1};
    static constexpr int V_NEG[6] = {0, 0, 1, 0, 0, 0};
    float ox = b.x0, oy = b.y0, oz = b.z0;
    float sx = b.x1 - b.x0, sy = b.y1 - b.y0, sz = b.z1 - b.z0;
    const auto uv = material ? material->uv[face] : renderlib::UVRect{0, 0, 1, 1};
    for (int v = 0; v < 4; ++v) {
        float local[3] = {FV[face][v][0], FV[face][v][1], FV[face][v][2]};
        float u = local[U_AXIS[face]];
        float tv = local[V_AXIS[face]];
        if (U_NEG[face]) u = 1.0f - u;
        if (V_NEG[face]) tv = 1.0f - tv;
        addVert(mesh,
                ox + local[0] * sx, oy + local[1] * sy, oz + local[2] * sz,
                FN[face][0], FN[face][1], FN[face][2], color,
                uv.u0 + (uv.u1 - uv.u0) * u,
                uv.v0 + (uv.v1 - uv.v0) * tv * v_len);
    }
    addQuad(mesh, vertBase, vertBase+1, vertBase+2, vertBase+3);
}

int emitBox(ChunkMeshBuilder::MeshData& mesh,
            const Box& b, const uint8_t* color,
            const PipeMeshMaterial* material,
            FaceMask hiddenFaces = 0) {
    static constexpr FaceMask FACE_MASKS[6] = {
        FACE_EAST, FACE_WEST, FACE_UP, FACE_DOWN, FACE_SOUTH, FACE_NORTH,
    };
    int base = static_cast<int>(mesh.vertices.size());
    int writtenFaces = 0;
    for (int f = 0; f < 6; ++f) {
        if (hiddenFaces & FACE_MASKS[f]) continue;
        emitBoxFace(mesh, b, f, color, base + writtenFaces * 4, material, 1.0f);
        ++writtenFaces;
    }
    return base;
}

void emitTube(ChunkMeshBuilder::MeshData& mesh,
              const Box& b, int face,
              const uint8_t* color, float v_len,
              const PipeMeshMaterial* material,
              bool includeEndCap) {
    int skip = TUBE_SKIP_FACE[face];
    int vi = static_cast<int>(mesh.vertices.size());
    for (int f = 0; f < 6; ++f) {
        if (f == skip || (f == face && !includeEndCap)) continue;
        emitBoxFace(mesh, b, f, color, vi, material, (f == face) ? 1.0f : v_len);
        vi += 4;
    }
}

void emitFlange(ChunkMeshBuilder::MeshData& mesh,
                const Box& b, int face,
                const uint8_t* color,
                const PipeMeshMaterial* material) {
    // The tube already owns the terminal cap. Emitting the same cap from the
    // larger flange puts two coplanar quads at every connection and flickers.
    emitTube(mesh, b, face, color, 1.0f, material, false);
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
        case PipeType::HEAT_PIPE:        out[0]=0xCC; out[1]=0x50; out[2]=0x30; out[3]=0xFF; break;
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
    // Start each tube at the junction boundary. The old geometry extended
    // into the center box, leaving coplanar side faces that z-fight.
    dg.tube[0]    = {gx+1.0f-j, gy+fi, gz+fi, gx+1.0f, gy+1.0f-fi, gz+1.0f-fi};
    dg.flange[0]  = {gx+1.0f-ft, gy+fo, gz+fo, gx+1.0f, gy+1.0f-fo, gz+1.0f-fo};
    dg.tube[1]    = {gx+0.0f, gy+fi, gz+fi, gx+j, gy+1.0f-fi, gz+1.0f-fi};
    dg.flange[1]  = {gx+0.0f, gy+fo, gz+fo, gx+ft, gy+1.0f-fo, gz+1.0f-fo};
    dg.tube[2]    = {gx+fi, gy+1.0f-j, gz+fi, gx+1.0f-fi, gy+1.0f, gz+1.0f-fi};
    dg.flange[2]  = {gx+fo, gy+1.0f-ft, gz+fo, gx+1.0f-fo, gy+1.0f, gz+1.0f-fo};
    dg.tube[3]    = {gx+fi, gy+0.0f, gz+fi, gx+1.0f-fi, gy+j, gz+1.0f-fi};
    dg.flange[3]  = {gx+fo, gy+0.0f, gz+fo, gx+1.0f-fo, gy+ft, gz+1.0f-fo};
    dg.tube[4]    = {gx+fi, gy+fi, gz+1.0f-j, gx+1.0f-fi, gy+1.0f-fi, gz+1.0f};
    dg.flange[4]  = {gx+fo, gy+fo, gz+1.0f-ft, gx+1.0f-fo, gy+1.0f-fo, gz+1.0f};
    dg.tube[5]    = {gx+fi, gy+fi, gz+0.0f, gx+1.0f-fi, gy+1.0f-fi, gz+j};
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
    std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock,
    std::function<uint8_t(int32_t, int32_t, int32_t)> getMeta) {
    FaceMask mask = 0;
    uint16_t target = pipeTypeToBlockId(type);
    if (getBlock(x,   y+1, z  ) == target) mask |= FACE_UP;
    if (getBlock(x,   y-1, z  ) == target) mask |= FACE_DOWN;
    if (getBlock(x,   y,   z-1) == target) mask |= FACE_NORTH;
    if (getBlock(x,   y,   z+1) == target) mask |= FACE_SOUTH;
    if (getBlock(x-1, y,   z  ) == target) mask |= FACE_WEST;
    if (getBlock(x+1, y,   z  ) == target) mask |= FACE_EAST;
    // Any machine block on a face draws a connection flange: machines are the
    // endpoints pipes attach to (boiler, generator, extractor...).
    if (isMachineBlock(getBlock(x,   y+1, z  ))) mask |= FACE_UP;
    if (isMachineBlock(getBlock(x,   y-1, z  ))) mask |= FACE_DOWN;
    if (isMachineBlock(getBlock(x,   y,   z-1))) mask |= FACE_NORTH;
    if (isMachineBlock(getBlock(x,   y,   z+1))) mask |= FACE_SOUTH;
    if (isMachineBlock(getBlock(x-1, y,   z  ))) mask |= FACE_WEST;
    if (isMachineBlock(getBlock(x+1, y,   z  ))) mask |= FACE_EAST;
    if (!getMeta) return mask;
    uint8_t mv = getMeta(x, y, z);
    if (mv == 0) return mask;  // legacy: unset meta = all 6 faces connected
    return mask & metaToFaceMask(mv);
}

ChunkMeshBuilder::MeshData PipeMeshBuilder::buildPipeMesh(
    int32_t x, int32_t y, int32_t z,
    PipeType type, FaceMask connections,
    const PipeMeshMaterial* material,
    FaceMask terminalFaces) {
    ChunkMeshBuilder::MeshData mesh;
    bool isCable = isCableType(type);

    mesh.vertices.reserve(isCable ? 150 : 300);
    mesh.indices.reserve(isCable ? 300 : 600);

    uint8_t color[4];
    pipeColor(type, color);
    if (material) {
        color[0] = color[1] = color[2] = color[3] = 255;
    }

    float gx = static_cast<float>(x);
    float gy = static_cast<float>(y);
    float gz = static_cast<float>(z);

    float jh = isCable ? C_JUNC : P_JUNC;
    Box center = {gx+jh, gy+jh, gz+jh, gx+1.0f-jh, gy+1.0f-jh, gz+1.0f-jh};
    // Connection tubes own the continuation of these faces. Hide the center
    // face on connected directions to avoid coplanar overlap at the junction.
    emitBox(mesh, center, color, material, connections);

    DirGeom dg = makeDirGeom(gx, gy, gz, !isCable);
    float tubeLen = 1.0f - jh - jh;

    for (int f = 0; f < 6; ++f) {
        if (!(connections & FACE_TO_MASK[f])) continue;
        // Connected pipes share this boundary; neither side needs a terminal
        // cap. Keeping one here would z-fight with the neighbor's cap.
        const bool terminal = (terminalFaces & FACE_TO_MASK[f]) != 0;
        emitTube(mesh, dg.tube[f], f, color, tubeLen, material, terminal);
        emitFlange(mesh, dg.flange[f], f, color, material);
    }

    return mesh;
}
