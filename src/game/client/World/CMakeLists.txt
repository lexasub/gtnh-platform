# Unit tests for the client-side interaction gate and the player body
# controller (beads gp-05vj).
#
# Uses the PROJECT's own CHECK/TEST harness (see
# src/engine/net/test/test.h, mirrored by
# src/game/world/test/BlockTransforms_test.cpp and
# src/game/machines/test/test_explosion_system.cpp). GoogleTest is deliberately
# NOT used: it is absent from conanfile.txt, CI does not install libgtest-dev,
# and CI builds Release with a global -Werror. A find_package(GTest QUIET)
# guard would be worse than nothing — the test would silently stop registering
# on CI instead of failing.
#
# No display, GPU, window, input device or wall-clock waiting is involved: the
# tests call the pure functions with a plain Camera (identity orientation),
# a struct InputState, and a default-constructed NetClient that drops its
# writes while no control connection is up.
#
# Sources are compiled directly into the test binary rather than linking
# gameclientd's object graph, and RenderLib/Utils/Raycaster.cpp is compiled in
# place of linking `renderlib` so the test never pulls in bgfx/bx/bimg or a
# GL context. This is the same pattern the existing gameclient_wrench_*_test
# targets use for their single production .cpp.

if(NOT BUILD_TESTING)
    return()
endif()

add_executable(gameclient_interaction_system_test
    test/InteractionSystem_test.cpp
    ${CMAKE_SOURCE_DIR}/src/game/client/World/InteractionSystem.cpp
    ${CMAKE_SOURCE_DIR}/src/game/client/Player/PlayerController.cpp
    ${CMAKE_SOURCE_DIR}/src/game/client/World/World.cpp
    ${CMAKE_SOURCE_DIR}/src/apps/game_client/Camera/Camera.cpp
    ${CMAKE_SOURCE_DIR}/src/apps/game_client/Network/NetClient.cpp
    ${CMAKE_SOURCE_DIR}/src/apps/game_client/World/ChunkStorage.cpp
    ${CMAKE_SOURCE_DIR}/src/apps/game_client/World/ChunkView.cpp
    ${CMAKE_SOURCE_DIR}/src/apps/game_client/RenderLib/Utils/Raycaster.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ui/client/core/InputBinder.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ui/client/core/ActionRegistry.cpp
)

target_compile_definitions(gameclient_interaction_system_test PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data"
)

target_include_directories(gameclient_interaction_system_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/apps/game_client
    ${CMAKE_SOURCE_DIR}/src/game/client
    ${CMAKE_SOURCE_DIR}/src/game/client/Common
    ${CMAKE_SOURCE_DIR}/src/game/ui/client
    ${CMAKE_SOURCE_DIR}/src/apps/chunk_store
    ${CMAKE_SOURCE_DIR}/src/engine/net/include
    ${FBS_GEN_DIR}
)

target_link_libraries(gameclient_interaction_system_test PRIVATE
    chunkd_codec
    gtnh-net
    flatbuffers::flatbuffers
    glm::glm
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    glfw
    TBB::tbb
)

# InteractionSystem.cpp includes the generated FlatBuffers protocol headers.
add_dependencies(gameclient_interaction_system_test game_client_fbs)

add_test(NAME gameclient_interaction_system_test
         COMMAND gameclient_interaction_system_test)

