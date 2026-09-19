#pragma once

#include <cstdint>

namespace renderlib {

// Провайдер данных для мини-карты
class IMinimapDataProvider {
public:
  virtual ~IMinimapDataProvider() = default;

  // Заполняет буфер пикселей RGBA8 (размер size x size). Возвращает true, если
  // данные изменились (текстуру нужно обновить)
  virtual bool UpdateMinimapPixels(uint32_t *pixelBuffer, int size) = 0;

  // Позиция игрока в координатах пикселей (0..size-1) для отрисовки точки
  virtual void GetPlayerPixelPosition(int &outX, int &outY) const = 0;
};

} // namespace renderlib