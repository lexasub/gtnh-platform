#pragma once

#include "../RenderLib/Common/IMinimapDataProvider.h"
#include "../World/World.h"

class MinimapWorldAdapter : public renderlib::IMinimapDataProvider {
public:
  explicit MinimapWorldAdapter(World *world);
  bool UpdateMinimapPixels(uint32_t *pixelBuffer, int size) override;
  void GetPlayerPixelPosition(int &outX, int &outY) const override;
  void SetCameraPosition(const glm::vec3 &pos);

private:
  World *world_;
  glm::vec3 cameraPos_;
  int lastCenterX_ = 0, lastCenterZ_ = 0;
  // Кеш наличия блоков в чанках (опционально)
};