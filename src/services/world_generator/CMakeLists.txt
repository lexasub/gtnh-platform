set(CMAKE_CXX_STANDARD 26)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(worldgeneratord
    WorldGenerator.cpp
    GenerationQueue.cpp
    OreConfig.cpp
    OreGenerator.cpp
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
    # TODO: These need to be built from source or fetched via a proper dependency manager.
    #       Currently they reference prebuilt objects in a local third_party/ build tree.
    #       See README for dependency setup instructions.

)
