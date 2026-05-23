#include "ChunkStorage.h"
#include "ChunkView.h"
#include "../Common/Types.h"
#include <spdlog/spdlog.h>

ChunkStorage::ChunkStorage() = default;
ChunkStorage::~ChunkStorage() = default;

bool ChunkStorage::HasChunk(ChunkCoord c) const {
    return chunks_.count(MakeChunkKey(c)) > 0;
}

std::shared_ptr<const ChunkView> ChunkStorage::GetChunk(ChunkCoord c) const {
    ChunkMap::const_accessor cacc;
    if (chunks_.find(cacc, MakeChunkKey(c)))
        return cacc->second;
    return nullptr;
}

void ChunkStorage::RemoveChunk(ChunkCoord c) {
    chunks_.erase(MakeChunkKey(c));
}

void ChunkStorage::Clear() {
    chunks_.clear();
}

size_t ChunkStorage::Size() const {
    return chunks_.size();
}

std::shared_ptr<const ChunkView> ChunkStorage::StoreAndGetChunk(const ChunkCoord &c, std::shared_ptr<ChunkView> chunk) {
    ChunkMap::accessor acc;
    chunks_.emplace(acc, MakeChunkKey(c), std::move(chunk));
    return acc->second;
}
