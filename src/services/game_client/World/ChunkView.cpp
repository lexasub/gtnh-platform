#include "ChunkView.h"
#include "Storage/SectionCodec.h"
#include "Chunk/Chunk.h"
#include <spdlog/spdlog.h>

ChunkView::ChunkView(std::shared_ptr<std::vector<uint8_t>> compressed)
    : compressed_(std::move(compressed))
{}

ChunkView::~ChunkView() = default;

void ChunkView::ensureFlat() const {
    if (flat_) return;
    flat_ = std::make_unique<Chunk>();
    bool ok = decodeChunk(compressed_->data(), compressed_->size(), *flat_);
    if (!ok) {
        spdlog::error("ChunkView: decodeChunk failed");
        return;
    }
}

uint16_t ChunkView::GetBlock(int x, int y, int z) const {
    ensureFlat();
    if (!flat_) return 0;
    int idx = (y << 10) | (z << 5) | x;
    return flat_->blocks[idx];
}

void ChunkView::SetBlock(int x, int y, int z, uint16_t block_id, uint8_t new_meta, uint32_t mb_id) const {
    ensureFlat();
    if (!flat_) return;
    int idx = (y << 10) | (z << 5) | x;
    flat_->blocks[idx] = block_id;
    flat_->meta[idx] = new_meta;
    flat_->multiblock[idx] = mb_id;
}

const uint16_t* ChunkView::blocks_data() const {
    ensureFlat();
    return flat_ ? flat_->blocks.data() : nullptr;
}

const uint8_t* ChunkView::meta_data() const {
    ensureFlat();
    return flat_ ? flat_->meta.data() : nullptr;
}

const uint32_t* ChunkView::multiblock_data() const {
    ensureFlat();
    return flat_ ? flat_->multiblock.data() : nullptr;
}
