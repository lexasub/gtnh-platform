cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(validationd
    main.cpp
    src/validation.cpp
)

target_link_libraries(validationd PRIVATE
    spdlog::spdlog
    flatbuffers::flatbuffers
)
