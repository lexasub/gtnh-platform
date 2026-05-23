#pragma once
#include <bgfx/bgfx.h>
#include <cstdint>

#pragma pack(push, 1)
struct BlockVertex {
    float x, y, z;       // global coords (baked in BuildGeometry)
    uint8_t normal[4];
    uint8_t color[4];
    float u, v;
};
#pragma pack(pop)

static_assert(sizeof(BlockVertex) == 28, "BlockVertex size must match bgfx vertex stride");

inline bgfx::VertexLayout BlockVertexLayout() {
    bgfx::VertexLayout l;
    l.begin()
        .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal,    4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float, false)
        .end();
    return l;
}

inline const bgfx::VertexLayout& BlockVertexLayoutRef() {
    static auto l = BlockVertexLayout();
    return l;
}