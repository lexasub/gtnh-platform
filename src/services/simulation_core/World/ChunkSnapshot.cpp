#include "ChunkSnapshot.h"
#include "core_generated.h"
#include "chunkstore_generated.h"
#include "Common/xyz.h"
#include <cstring>

namespace simcore {

    ChunkSnapshot::ChunkSnapshot(const uint8_t* data, size_t len)
        : data_(data, data + len)
    {
        // Разбираем flatbuffers сообщение
        auto snapshot = flatbuffers::GetRoot<Protocol::BlockChangedEvent>(data_.data());
        if (!snapshot) {
            // пустой снимок, координаты нулевые
            coord_ = {0,0,0};
            return;
        }

        auto coord_fb = snapshot->pos();
        if (coord_fb) {
            coord_.x = coord_fb->x();
            coord_.y = coord_fb->y();
            coord_.z = coord_fb->z();
        } else {
            coord_ = {0,0,0};
        }
        /*
        auto blocks_fb = snapshot->blocks();
        auto meta_fb   = snapshot->meta();
        auto mb_fb     = snapshot->multiblock();

        if (blocks_fb) {
            blocks_.assign(blocks_fb->begin(), blocks_fb->end());
        }
        if (meta_fb) {
            meta_.assign(meta_fb->begin(), meta_fb->end());
        }
        if (mb_fb) {
            multiblock_.assign(mb_fb->begin(), mb_fb->end());
        }
        */
    }

    ChunkCoord ChunkSnapshot::coord() const
    {
        return coord_;
    }

    uint16_t ChunkSnapshot::getBlock(uint32_t x, uint32_t y, uint32_t z) const
    {
        uint32_t idx = xyz(x, y, z);  // предполагается, что чанк 32x32x32, макс индекс ~32768
        if (idx < blocks_.size()) {
            return blocks_[idx];
        }
        return 0;
    }

    uint8_t ChunkSnapshot::getMeta(uint32_t x, uint32_t y, uint32_t z) const
    {
        uint32_t idx = xyz(x, y, z);
        if (idx < meta_.size()) {
            return meta_[idx];
        }
        return 0;
    }

    uint32_t ChunkSnapshot::getMultiblock(uint32_t x, uint32_t y, uint32_t z) const
    {
        uint32_t idx = xyz(x, y, z);
        if (idx < multiblock_.size()) {
            return multiblock_[idx];
        }
        return 0;
    }

    size_t ChunkSnapshot::size() const
    {
        return data_.size();
    }

    const uint8_t* ChunkSnapshot::data() const
    {
        return data_.data();
    }

} // namespace simcore