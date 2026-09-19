#pragma once

#include <array>
#include <cstdint>

struct InputState {
  static constexpr int kMaxCharsPerFrame = 16;
  std::array<uint32_t, kMaxCharsPerFrame> charBuf{};
  int charCount = 0;

  std::array<bool, 512> keys{};

  bool mouseLeft = false;
  bool mouseRight = false;
  bool mouseLeftPressed = false; // true only on the frame of press
  bool mouseRightPressed = false;

  double mouseX = 0.0, mouseY = 0.0;
  double mouseDX = 0.0, mouseDY = 0.0;
  double scrollY = 0.0;

  void ResetFrameState() {
    mouseDX = 0.0;
    mouseDY = 0.0;
    mouseLeftPressed = false;
    mouseRightPressed = false;
    scrollY = 0.0;
    charCount = 0;
  }
};