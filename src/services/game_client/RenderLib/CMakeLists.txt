# RenderLib — standalone bgfx rendering library for game_client
# Depends on: bgfx, glm, glfw (public), spdlog, imgui, TBB (private)

add_library(renderlib STATIC
    Common/RenderAPI.cpp
    Scene/RenderScene.cpp
    Scene/Frustum.cpp
    UI/Minimap.cpp
    UI/ImGuiBackend.cpp
    Utils/TextureAtlas.cpp
    Utils/ShaderLoader.cpp
    Utils/Raycaster.cpp
)

# PUBLIC = game_client root, so consumers include as: #include "RenderLib/Common/RenderAPI.h"
target_include_directories(renderlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Dependencies exposed via public headers
target_link_libraries(renderlib PUBLIC
    bgfx bx bimg
    glm::glm
    glfw
    imgui::imgui
)

# Dependencies used only in implementation (.cpp) files
target_link_libraries(renderlib PRIVATE
    spdlog::spdlog
    TBB::tbb
)
