#include "MinimapWorldAdapter.h"
#include "../World/ChunkView.h"
#include <cmath>
#include <algorithm>
#include <spdlog/spdlog.h>

MinimapWorldAdapter::MinimapWorldAdapter(World* world) : world_(world) {}

void MinimapWorldAdapter::SetCameraPosition(const glm::vec3& pos) {
    cameraPos_ = pos;
}

bool MinimapWorldAdapter::UpdateMinimapPixels(uint32_t* pixelBuffer, int size) {
    if (!world_ || size <= 0) return false;
    // Определяем центр по камере
    int cx = static_cast<int>(std::floor(cameraPos_.x / CHUNK_SIZE));
    int cz = static_cast<int>(std::floor(cameraPos_.z / CHUNK_SIZE));
    // Если центр не изменился и прошло мало времени, можно не обновлять
    const int viewRadius = 6;
    const int gridSize = 2 * viewRadius + 1;
    const int cellSize = size / gridSize;
    if (cellSize < 1) return false;

    if (cx == lastCenterX_ && cz == lastCenterZ_) {
        // Можно добавить таймер обновления, но пока пропускаем
        return false;
    }
    lastCenterX_ = cx;
    lastCenterZ_ = cz;

    // Заполняем чёрным
    std::fill_n(pixelBuffer, size * size, 0xFF202020);

    for (int dz = -viewRadius; dz <= viewRadius; ++dz) {
        for (int dx = -viewRadius; dx <= viewRadius; ++dx) {
            ChunkCoord coord{cx + dx, 0, cz + dz};
            auto chunk = world_->GetChunk(coord);
            if (!chunk) continue;
            // Быстрая проверка наличия не-air блоков (можно закешировать)
            const uint16_t* blocks = chunk->blocks_data();
            bool hasBlocks = false;
            for (size_t i = 0; i < chunk->blocks_size(); ++i) {
                if (blocks[i] != 0) { hasBlocks = true; break; }
            }
            if (!hasBlocks) continue;

            int screenX = (dx + viewRadius) * cellSize + 1;
            int screenY = (dz + viewRadius) * cellSize + 1;
            int startX = std::max(0, screenX);
            int endX = std::min(size, screenX + cellSize - 2);
            int startY = std::max(0, screenY);
            int endY = std::min(size, screenY + cellSize - 2);
            uint32_t color = 0xFF00FF00; // зелёный для наличия чанка
            for (int y = startY; y < endY; ++y) {
                for (int x = startX; x < endX; ++x) {
                    pixelBuffer[y * size + x] = color;
                }
            }
        }
    }
    return true;
}

void MinimapWorldAdapter::GetPlayerPixelPosition(int& outX, int& outY) const {
    // Рассчитываем позицию игрока в пикселях мини-карты (упрощённо)
    const int viewRadius = 6;
    const int gridSize = 2 * viewRadius + 1;
    const int cellSize = 256 / gridSize; // size = 256
    int cx = static_cast<int>(std::floor(cameraPos_.x / CHUNK_SIZE));
    int cz = static_cast<int>(std::floor(cameraPos_.z / CHUNK_SIZE));
    float localX = cameraPos_.x - cx * CHUNK_SIZE;
    float localZ = cameraPos_.z - cz * CHUNK_SIZE;
    int px = static_cast<int>((localX / CHUNK_SIZE) * cellSize + viewRadius * cellSize);
    int py = static_cast<int>((localZ / CHUNK_SIZE) * cellSize + viewRadius * cellSize);
    outX = std::clamp(px, 0, 255);
    outY = std::clamp(py, 0, 255);
}