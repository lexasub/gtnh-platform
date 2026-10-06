# RenderLib — standalone bgfx rendering library for game_client
# Depends on: bgfx, glm, glfw (public), spdlog, imgui, TBB (private)
find_package(lodepng REQUIRED)
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
target_include_directories(renderlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${lodepng_INCLUDE_DIRS})

# TextureAtlas is compiled as part of RenderLib, so the runtime data root must
# be defined on this target (gameclientd's definition is not inherited).
target_compile_definitions(renderlib PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data"
)


# Dependencies exposed via public headers
target_link_libraries(renderlib PUBLIC
    bgfx bx bimg
    glm::glm
    glfw
    imgui::imgui
    lodepng::lodepng
)

# Dependencies used only in implementation (.cpp) files
target_link_libraries(renderlib PRIVATE
    spdlog::spdlog
    TBB::tbb
)

if(BUILD_TESTING)
    # Compile the atlas against an isolated synthetic data directory so the
    # highest tile and physical-slot boundary can be exercised without changing
    # the canonical content registries.
    add_executable(gameclient_texture_atlas_full_boundary_test
        ../tests/TextureAtlasFullBoundary_test.cpp
        Utils/TextureAtlas.cpp
    )
    target_compile_definitions(gameclient_texture_atlas_full_boundary_test PRIVATE
        DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/texture_atlas_full_fixture"
    )
    target_include_directories(gameclient_texture_atlas_full_boundary_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
    target_link_libraries(gameclient_texture_atlas_full_boundary_test PRIVATE
        bgfx bx bimg lodepng::lodepng spdlog::spdlog
    )
    add_test(NAME gameclient_texture_atlas_full_boundary_test
             COMMAND gameclient_texture_atlas_full_boundary_test)
endif()
