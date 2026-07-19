#include "ChunkView.h"
#include "Storage/cache/MutableChunk.h"
#include <spdlog/spdlog.h>

ChunkView::ChunkView(std::shared_ptr<std::vector<uint8_t>> wire_data)
    : wire_data_(std::move(wire_data))
{}

ChunkView::~ChunkView() = default;

void ChunkView::ensureFlat() const {
    if (flat_) return;
    flat_ = std::make_unique<MutableChunk>();
    if (!wire_data_ || wire_data_->empty()) return;
    bool ok = flat_->fromWire(wire_data_->data(), wire_data_->size());
    if (!ok) {
        spdlog::error("ChunkView: fromWire failed");
        flat_.reset();
        return;
    }
    wire_data_.reset();
}

void ChunkView::ensureFlatArrays() const {
    if (flat_blocks_) return;
    ensureFlat();
    if (!flat_) return;
    flat_blocks_ = std::make_unique<uint16_t[]>(VOLUME);
    flat_meta_ = std::make_unique<uint8_t[]>(VOLUME);
    flat_mb_ = std::make_unique<uint32_t[]>(VOLUME);
    for (int y = 0; y < 32; ++y)
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 32; ++x) {
                int idx = (y << 10) | (z << 5) | x;
                flat_blocks_[idx] = flat_->getBlock(x, y, z);
                flat_meta_[idx] = flat_->getMeta(x, y, z);
                flat_mb_[idx] = flat_->getMultiblock(x, y, z);
            }
}

uint16_t ChunkView::GetBlock(int x, int y, int z) const {
    ensureFlat();
    if (!flat_) return 0;
    return flat_->getBlock(x, y, z);
}

void ChunkView::SetBlock(int x, int y, int z, uint16_t block_id, uint8_t meta,
                         uint32_t mb_id) const {
    ensureFlat();
    if (!flat_) return;
    flat_->setBlock(x, y, z, block_id);
    flat_->setMeta(x, y, z, meta);
    flat_->setMultiblock(x, y, z, mb_id);
}

const uint16_t* ChunkView::blocks_data() const {
    ensureFlatArrays();
    return flat_blocks_.get();
}

const uint8_t* ChunkView::meta_data() const {
    ensureFlatArrays();
    return flat_meta_.get();
}

const uint32_t* ChunkView::multiblock_data() const {
    ensureFlatArrays();
    return flat_mb_.get();
}
