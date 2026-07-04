#pragma once

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

// Template function for rendering paginated lists with any item type.
// page is updated by ref on Previous/Next button clicks.
template <typename T, typename F>
inline void RenderPaginatedList(const std::vector<T> &items, int &page,
                                int perPage, F &&renderItem,
                                const char *emptyMessage = "No items found") {
  if (items.empty()) {
    ImGui::Text("%s", emptyMessage);
    return;
  }
  int total = static_cast<int>(items.size());
  int maxPage = (total - 1) / perPage;
  if (page > maxPage)
    page = maxPage;
  int start = page * perPage;
  int end = std::min(start + perPage, total);

  // Page counter
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%d / %d  (%d total)", page + 1, maxPage + 1,
                total);
  ImGui::TextUnformatted(buf);
  ImGui::SameLine();
  if (page > 0) {
    if (ImGui::SmallButton("<")) {
      page--;
    }
    ImGui::SameLine();
  }
  if (page < maxPage) {
    if (ImGui::SmallButton(">")) {
      page++;
    }
  }
  ImGui::Separator();

  ImGui::BeginChild("entryList", ImVec2(0, 0), true);
  for (int ei = start; ei < end; ++ei) {
    ImGui::PushID(ei);
    renderItem(items[ei], ei);
    ImGui::PopID();
  }
  ImGui::EndChild();
}
