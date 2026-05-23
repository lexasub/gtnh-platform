#include "Minimap.h"
#include "../Common/IMinimapDataProvider.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>

namespace renderlib {

    Minimap::Minimap() : texture_(BGFX_INVALID_HANDLE), pixelBuffer_(std::make_unique<uint32_t[]>(size_ * size_)) {
        std::fill_n(pixelBuffer_.get(), size_ * size_, 0xFF202020);
    }

    Minimap::~Minimap() {
        if (bgfx::isValid(texture_)) bgfx::destroy(texture_);
    }

    void Minimap::Render(IMinimapDataProvider* provider) {
        if (!provider) return;
        if (!bgfx::isValid(texture_)) {
            texture_ = bgfx::createTexture2D(static_cast<uint16_t>(size_), static_cast<uint16_t>(size_),
                                             false, 1, bgfx::TextureFormat::RGBA8,
                                             BGFX_TEXTURE_NONE | BGFX_SAMPLER_POINT, nullptr);
            if (!bgfx::isValid(texture_)) {
                spdlog::error("Minimap: failed to create texture");
                return;
            }
        }
        bool updated = provider->UpdateMinimapPixels(pixelBuffer_.get(), size_);
        if (updated) {
            UploadTexture();
            textureValid_ = true;
        }
        provider->GetPlayerPixelPosition(playerX_, playerY_);
    }

    void Minimap::UploadTexture() {
        if (!bgfx::isValid(texture_)) return;
        bgfx::updateTexture2D(texture_, 0, 0, 0, 0,
                              static_cast<uint16_t>(size_), static_cast<uint16_t>(size_),
                              bgfx::copy(pixelBuffer_.get(), size_ * size_ * 4));
    }

    void Minimap::DrawImGui(int /*width*/, int height) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, static_cast<float>(height) - static_cast<float>(size_)),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(size_), static_cast<float>(size_)),
                                 ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("Minimap", nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
        ImGui::PopStyleVar();
        ImVec2 mmSz(static_cast<float>(size_), static_cast<float>(size_));
        if (!bgfx::isValid(texture_)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p0, ImVec2(p0.x + mmSz.x, p0.y + mmSz.y), IM_COL32(255, 0, 0, 200));
            ImGui::Dummy(mmSz);
        } else {
            ImVec2 imgOrg = ImGui::GetCursorScreenPos();
            // bgfx texture handle idx is used as ImTextureID
            ImGui::Image(ImTextureID(static_cast<ImTextureID>(texture_.idx)), mmSz);
            float sx = mmSz.x / static_cast<float>(size_);
            float sy = mmSz.y / static_cast<float>(size_);
            ImVec2 dot(imgOrg.x + playerX_ * sx, imgOrg.y + playerY_ * sy);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(dot, 4.0f, IM_COL32(255, 0, 0, 255));
        }
        ImGui::End();
    }

} // namespace renderlib