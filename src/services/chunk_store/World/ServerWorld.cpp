#include "ServerWorld.h"
#include "../Storage/ChunkStore.h"
#include "../../world_generator/WorldGenerator.h"
#include "../../world_generator/GenerationQueue.h"
#include <spdlog/spdlog.h>

ServerWorld::ServerWorld()
    : worldId_(0), generator_(nullptr) {
}

ServerWorld::~ServerWorld() {
}

void ServerWorld::Init(int worldId, std::string dbPath, size_t cache_size, size_t max_map_size) {
    worldId_ = worldId;
    block_store_.reset(chunkStore_ = new ChunkStore(dbPath, cache_size, max_map_size));
    generator_.reset(new WorldGenerator());

    // Create generation queue and wire it to ChunkStore
    gen_queue_ = std::make_unique<GenerationQueue>(generator_.get(), [this](ChunkCoord coord, std::shared_ptr<Chunk> chunk) {
        chunkStore_->enqueueEncode(coord, std::move(chunk)); //TODO use func ptr instead lambda
    });
    chunkStore_->SetGenerationQueue(gen_queue_.get()); //TODO refactor bidi subscribe

    spdlog::info("ServerWorld {} initialized with ChunkStore and WorldGenerator", worldId);
}

void ServerWorld::SetBlock(BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId) {
    ChunkCoord coord{pos.x >> 5, pos.y >> 5, pos.z >> 5};
    block_store_->SetBlock(coord, pos, blockId, meta, mbId);
}

uint16_t ServerWorld::GetBlockAt(BlockPos pos) const {
    return block_store_->GetBlockAt(pos);
}

void ServerWorld::SaveChunk(const Chunk& chunk, const ChunkCoord &coord) {
    block_store_->SaveChunk(chunk, coord);
}

const Chunk* ServerWorld::GetChunk(const ChunkCoord &coord) {
    // Try to get from store first
    const Chunk* chunk = block_store_->GetChunk(coord);
    
    // If chunk is empty (all zeros), generate it procedurally
    if (!chunk) {
        return nullptr;
    }

    bool isEmpty = true;
    for (const auto& block : chunk->blocks) {
        if (block != 0) {
            isEmpty = false;
            break;
        }
    }

    if (!isEmpty) {
        return chunk;
    }

    spdlog::info("ServerWorld: Generating chunk at ({}, {}, {}) procedurally", coord.x, coord.y, coord.z);

    // Create a new chunk and generate terrain
    Chunk generatedChunk;
    generator_->GenerateTerrain(generatedChunk, coord.x, coord.y, coord.z);

    // Save to store (this now also updates the cache)
    block_store_->SaveChunk(generatedChunk, coord);

    // Get the chunk again - it should now be in cache with generated data
    chunk = block_store_->GetChunk(coord);

    return chunk;
}

void ServerWorld::AsyncGetChunk(ChunkCoord coord,
                                ChunkStore::ChunkCallback callback) {
    chunkStore_->AsyncGetChunk(coord, std::move(callback));
}

void ServerWorld::SetBlockAsync(BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId,
                                std::function<void(bool)> callback) {
    // Delegate to ChunkStore's async method
    chunkStore_->AsyncSetBlock({pos.x >> 5, pos.y >> 5, pos.z >> 5}, pos, blockId, meta, mbId, callback);
}
ChunkStore* ServerWorld::GetChunkStore() const {
    return dynamic_cast<ChunkStore*>(block_store_.get());
}
