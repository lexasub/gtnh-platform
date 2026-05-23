add_executable(spatialindexd main.cpp)
target_link_libraries(spatialindexd PRIVATE
    flatbuffers::flatbuffers
    glm::glm
    #boost::boost
    spdlog::spdlog
)
