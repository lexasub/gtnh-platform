cmake_minimum_required(VERSION 3.20)
project(GTNHPlatform CXX C)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler warnings (global for all targets)
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    #add_compile_options(-g -fsanitize=address -fsanitize=undefined)
    #add_link_options(-fsanitize=address -fsanitize=undefined)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-Werror)
endif()

# CTest integration
include(CTest)
enable_testing()


set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "Build Noise Tool" FORCE)

# bgfx compile flags\n#add_compile_definitions(BGFX_CONFIG_DEBUG=1)\n\n# Export compile_commands.json for LSP / clang-tidy\nset(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\n# Include paths for liburing\ninclude_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/services/\\n    chunk_store\\n    third_party/bgfx.cmake/include)\n\n# Auto-detect Conan toolchain (works with any build dir, including IDE-created ones)
if(NOT CMAKE_TOOLCHAIN_FILE)
    # Pick conan directory matching build type
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_conan_dirs "conan_debug" "conan" ".")
    else()
        set(_conan_dirs "conan" ".")
    endif()
    foreach(_conan_dir ${_conan_dirs})
        if(EXISTS "${CMAKE_SOURCE_DIR}/${_conan_dir}/conan_toolchain.cmake")
            set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/${_conan_dir}/conan_toolchain.cmake")
            break()
        endif()
    endforeach()
endif()
if(CMAKE_TOOLCHAIN_FILE)
    include(${CMAKE_TOOLCHAIN_FILE})
endif()

# Conan-managed dependencies
find_package(asio REQUIRED)
find_package(flatbuffers REQUIRED)
find_package(glm REQUIRED)
find_package(EnTT REQUIRED)
find_package(miniaudio REQUIRED)
find_package(spdlog REQUIRED)
find_package(glfw3 REQUIRED)
#find_package(Boost REQUIRED)
find_package(TBB REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(lmdb REQUIRED)

find_package(FastNoise2 REQUIRED)

# Shared libraries
add_subdirectory(src/libs)

# Service subdirectories
#add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/-.cmake)

#find_package(bgfx REQUIRED)
find_package(imgui CONFIG REQUIRED)

#add_subdirectory(third_party/bgfx.cmake)

#add_subdirectory(third_party/FastNoise2)
add_subdirectory(src/services/gateway)
add_subdirectory(src/services/chunk_store)
add_subdirectory(src/services/world_generator)
add_subdirectory(src/services/simulation_core)
add_subdirectory(src/services/pipe_network)
#add_subdirectory(src/services/spatial_index)
add_subdirectory(src/services/meta_db)
add_subdirectory(src/services/game_client)
add_subdirectory(src/services/entity_state_store)
add_subdirectory(src/services/recipe_manager)

#add_subdirectory(src/services/message_router)

#find_package(GTest CONFIG REQUIRED)
