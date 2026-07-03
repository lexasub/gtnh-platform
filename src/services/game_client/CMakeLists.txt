# FlatBuffers schema → C++ headers — shared protocol from src/protocol/
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/game_client_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/gateway_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
        ${FBS_GEN_DIR}/quest_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/gateway.fbs ${FBS_DIR}/recipe.fbs ${FBS_DIR}/quest.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/gateway.fbs
            ${FBS_DIR}/recipe.fbs
            ${FBS_DIR}/quest.fbs
    COMMENT "Generating FlatBuffers headers for game_client"
)

add_custom_target(game_client_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/gateway_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
        ${FBS_GEN_DIR}/quest_generated.h
)

# Set output directories for binaries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_CXX_STANDARD 26)
# Include generated FlatBuffers headers
include_directories(BEFORE SYSTEM ${flatbuffers_INCLUDE_DIRS} ${FBS_GEN_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src)

# RenderLib as a separate library target
add_subdirectory(RenderLib)
add_subdirectory(UI)

add_executable(gameclientd
    main.cpp

    GameClient.cpp

    Camera/Camera.cpp

    Crafting/ClientRecipeDB.cpp
    Crafting/ClientItemRegistry.cpp
    Crafting/ClientMachineRecipeDB.cpp

    Network/NetClient.cpp

    Cache/ChunkMeshCache.cpp
    Cache/ChunkNeighborCache.cpp
    Cache/ChunkLoadManager.cpp
    Cache/MeshManager.cpp

    Render/ChunkMeshBuilder.cpp
    Render/PipeMeshBuilder.cpp
    Render/CableMeshBuilder.cpp
    Render/MinimapWorldAdapter.cpp
    Render/ChunkMeshProvider.cpp
    Render/RenderBridge.cpp

    World/InteractionSystem.cpp
    World/ChunkStorage.cpp
    World/ChunkView.cpp
    World/World.cpp

)


include_directories(Common)

add_dependencies(gameclientd game_client_fbs)

# Find liburing (system library, no CMake config)
find_library(URING_LIB uring)
if(NOT URING_LIB)
    message(FATAL_ERROR "liburing not found — install liburing-dev")
endif()

find_package(nlohmann_json REQUIRED)

target_include_directories(gameclientd PRIVATE
    ${CMAKE_SOURCE_DIR}/src/services/chunk_store
)

target_link_libraries(gameclientd PRIVATE
    gtnh-net
    machine_registry
    quest_lib
    renderlib
    UI
    flatbuffers::flatbuffers
    miniaudio::miniaudio
    asio::asio
    nlohmann_json::nlohmann_json
    ${URING_LIB}
)

# Include generated bgfx headers
#target_include_directories(gameclientd PRIVATE
#    ${CMAKE_BINARY_DIR}/third_party/bgfx.cmake/cmake/bgfx/generated
#)

# Shader directory compile definition (for runtime loading)
target_compile_definitions(gameclientd PRIVATE
    SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/"
    DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)

# Enable bgfx multi-threaded mode: API thread != internal render thread
# This makes bgfx::frame() non-blocking and enables bgfx::Encoder from any thread.
target_compile_definitions(gameclientd PRIVATE
    BGFX_CONFIG_MULTITHREADED=1
)

# Optional profiling CSV log
option(PROFILE_LOG "Enable per-frame profiling CSV log" OFF)
if(PROFILE_LOG)
    target_compile_definitions(gameclientd PRIVATE PROFILE_LOG=1)
endif()

# ImGui includes (from bgfx bundled)
#target_include_directories(gameclientd PRIVATE
#    ${BGFX_DIR}/3rdparty
#    ${BGFX_DIR}/examples/common
#)

# ---- Shader compilation ----
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

set(BGFX_DIR ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake)

function(compile_shader src variant profile out_var)
    get_filename_component(name ${src} NAME_WE)
    set(output ${SHADER_OUT}/${name}.${variant}.bin)
    add_custom_command(
        OUTPUT ${output}
	COMMAND ${CMAKE_SOURCE_DIR}/shaderc
            -f ${src}
            -o ${output}
            --platform linux
            --type ${variant}
            --profile ${profile}
            -i ${SHADER_DIR}
            -i ${BGFX_DIR}/bgfx/src
            --verbose
        DEPENDS ${src}
        COMMENT "Compiling ${name}.${variant} shader"
    )
    set(${out_var} ${output} PARENT_SCOPE)
endfunction()

function(compile_shader_vulkan src variant out_var)
    get_filename_component(name ${src} NAME_WE)
    set(output ${SHADER_OUT}/${name}.${variant}.bin)
    add_custom_command(
        OUTPUT ${output}
	COMMAND ${CMAKE_SOURCE_DIR}/shaderc
            -f ${src}
            -o ${output}
            --platform linux
            --type ${variant}
            --profile spirv
            -i ${SHADER_DIR}
            -i ${BGFX_DIR}/bgfx/src
            --varyingdef ${SHADER_DIR}/varying.def.sc
            --verbose
        DEPENDS ${src}
        COMMENT "Compiling ${name}.${variant} Vulkan shader"
    )
    set(${out_var} ${output} PARENT_SCOPE)
endfunction()
#set(USE_VULKAN ON)
#if(USE_VULKAN)
    #compile_shader_vulkan(${SHADER_DIR}/vs_block.sc "vert" VS_BLOCK)
    #compile_shader_vulkan(${SHADER_DIR}/fs_block.sc "frag" FS_BLOCK)
#else()
    compile_shader(${SHADER_DIR}/vs_block.sc "vert" "120" VS_BLOCK)
    compile_shader(${SHADER_DIR}/fs_block.sc "frag" "120" FS_BLOCK)

    compile_shader(${SHADER_DIR}/vs_imgui.sc "vert" "120" VS_IMGUI)
    compile_shader(${SHADER_DIR}/fs_imgui.sc "frag" "120" FS_IMGUI)
#endif()

add_custom_target(game_client_shaders DEPENDS ${VS_BLOCK} ${FS_BLOCK} ${VS_IMGUI} ${FS_IMGUI})
add_dependencies(gameclientd game_client_shaders)

# Link against miniaudio's header-only interface
target_include_directories(gameclientd PRIVATE ${miniaudio_INCLUDE_DIRS} UI ${CMAKE_SOURCE_DIR}/src/libs)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(gameclient_item_registry_test
    tests/test_item_registry.cpp
    Crafting/ClientItemRegistry.cpp
)
target_compile_definitions(gameclient_item_registry_test PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)
target_include_directories(gameclient_item_registry_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME gameclient_item_registry_test COMMAND gameclient_item_registry_test)

add_executable(gameclient_drag_manager_test
    tests/DragManager_test.cpp
    UI/Core/DragManager.cpp
)
target_compile_definitions(gameclient_drag_manager_test PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)
target_include_directories(gameclient_drag_manager_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(gameclient_drag_manager_test PRIVATE
    glm::glm
)
add_test(NAME gameclient_drag_manager_test COMMAND gameclient_drag_manager_test)
