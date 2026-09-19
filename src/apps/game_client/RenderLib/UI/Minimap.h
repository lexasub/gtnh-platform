#pragma once

#include <bgfx/bgfx.h>
#include <memory>

namespace renderlib {
class IMinimapDataProvider;

class Minimap {
public:
  Minimap();
  ~Minimap();

  // Вызывается каждый кадр: обновляет текстуру, если нужно
  void Render(IMinimapDataProvider *provider);

  bgfx::TextureHandle GetTexture() const { return texture_; }
  int GetSize() const { return size_; }
  int GetPlayerPixelX() const { return playerX_; }
  int GetPlayerPixelY() const { return playerY_; }

  // ImGui rendering of minimap window
  void DrawImGui(int width, int height);

private:
  void UploadTexture();
  bgfx::TextureHandle texture_;
  int size_ = 256;
  std::unique_ptr<uint32_t[]> pixelBuffer_;
  int playerX_ = 0, playerY_ = 0;
  bool textureValid_ = false;
};

} // namespace renderlib