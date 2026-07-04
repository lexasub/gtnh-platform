#pragma once

#include <imgui.h>
#include <string>

// ── ToastMessage — timed colored notification ──────────────────────────────
// Stores a message with color and remaining lifetime.
// Call Render() each frame to display and auto-fade.
struct ToastMessage {
  std::string text;
  ImVec4 color = ImVec4(1, 1, 1, 1);
  float lifetime = 0.0f;

  bool IsActive() const { return lifetime > 0.0f && !text.empty(); }

  // Render the toast if active. Returns true while visible.
  bool Render() {
    if (lifetime <= 0.0f || text.empty()) {
      lifetime = 0.0f;
      text.clear();
      return false;
    }
    ImGui::TextColored(color, "%s", text.c_str());
    lifetime -= ImGui::GetIO().DeltaTime;
    if (lifetime <= 0.0f) {
      text.clear();
      lifetime = 0.0f;
    }
    return true;
  }
};
