set(CMAKE_CXX_STANDARD 26)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_compile_options(-g -O3 -march=native -ffast-math)

add_library(worldgeneratord
    WorldGenerator.cpp
    GenerationQueue.cpp
    OreConfig.cpp
    OreGenerator.cpp
    SurfaceHeights.cpp
    TreeGenerator.cpp
)
target_include_directories(worldgeneratord PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/FastNoise2/include
    ${CMAKE_SOURCE_DIR}/third_party/FastSIMD/include
)
target_include_directories(worldgeneratord PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(worldgeneratord PRIVATE
    glm::glm
    spdlog::spdlog
    Threads::Threads
    nlohmann_json::nlohmann_json
    fastnoise2::fastnoise2
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(worldgeneratord_test
    test/test_main.cpp
    test/test_tree_generation.cpp
)
target_compile_definitions(worldgeneratord_test PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)
target_include_directories(worldgeneratord_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/libs
    ${CMAKE_SOURCE_DIR}/third_party/FastNoise2/include
)
target_link_libraries(worldgeneratord_test PRIVATE
    worldgeneratord
    recipe_manager_lib
    spdlog::spdlog
    fastnoise2::fastnoise2
    glm::glm
    nlohmann_json::nlohmann_json
    Threads::Threads
    pthread
)
add_test(NAME worldgeneratord_test COMMAND worldgeneratord_test)
