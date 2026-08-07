#include "SurfaceHeights.h"

#include "FastNoise/FastNoise.h"

namespace {
    constexpr float BASE_FREQ  = 0.02f;
    constexpr float CONT_FREQ  = 0.005f;
    constexpr float BASE_AMP   = 12.0f;
    constexpr float CONT_AMP   = 20.0f;
    constexpr float BASE_HEIGHT = 64.0f;

    // Noise nodes — configured once per thread, identical to the
    // original WorldGenerator.cpp config so non-tree terrain is
    // byte-identical.
    thread_local FastNoise::SmartNode<FastNoise::Perlin>     basePerlin = FastNoise::New<FastNoise::Perlin>();
    thread_local FastNoise::SmartNode<FastNoise::FractalFBm> baseFBM    = FastNoise::New<FastNoise::FractalFBm>();

    thread_local FastNoise::SmartNode<FastNoise::Simplex>    contSimplex = FastNoise::New<FastNoise::Simplex>();
    thread_local FastNoise::SmartNode<FastNoise::FractalFBm> contFBM     = FastNoise::New<FastNoise::FractalFBm>();

    thread_local bool initialized = false;

    void init() {
        if (initialized) [[likely]] return;

        basePerlin->SetScale(1.0f / BASE_FREQ);
        baseFBM->SetSource(basePerlin);
        baseFBM->SetOctaveCount(3);
        baseFBM->SetLacunarity(2.0f);
        baseFBM->SetGain(0.5f);

        contSimplex->SetScale(1.0f / CONT_FREQ);
        contFBM->SetSource(contSimplex);
        contFBM->SetOctaveCount(2);
        contFBM->SetLacunarity(2.0f);
        contFBM->SetGain(0.5f);

        initialized = true;
    }
} // anonymous namespace

SurfaceHeights::SurfaceHeights(uint32_t worldSeed) : seed_(worldSeed) {}

void SurfaceHeights::fill(float* out, int size, int baseX, int baseZ) const {
    init();

    const int total = size * size;

    // Reuse thread_local buffers for the two 2D noise grids.
    // We size them generously — a typical caller passes size ≤ 36.
    thread_local std::vector<float> baseBuf;
    thread_local std::vector<float> contBuf;
    if (static_cast<int>(baseBuf.size()) < total) {
        baseBuf.resize(total);
        contBuf.resize(total);
    }

    baseFBM->GenUniformGrid2D(baseBuf.data(), baseX, baseZ, size, size,
                               1.0f, 1.0f, seed_);
    contFBM->GenUniformGrid2D(contBuf.data(), baseX, baseZ, size, size,
                               1.0f, 1.0f, seed_ + 1);

    for (int i = 0; i < total; ++i) {
        out[i] = BASE_HEIGHT + baseBuf[i] * BASE_AMP + contBuf[i] * CONT_AMP;
    }
}

float SurfaceHeights::at(int wx, int wz) const {
    init();

    // One-column fill call.
    float h;
    baseFBM->GenUniformGrid2D(&h, wx, wz, 1, 1, 1.0f, 1.0f, seed_);
    float c;
    contFBM->GenUniformGrid2D(&c, wx, wz, 1, 1, 1.0f, 1.0f, seed_ + 1);
    return BASE_HEIGHT + h * BASE_AMP + c * CONT_AMP;
}
