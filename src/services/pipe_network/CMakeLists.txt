cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(flatbuffers REQUIRED)
find_package(spdlog REQUIRED)
find_package(asio REQUIRED)
find_package(Threads REQUIRED)

# FlatBuffers schema → C++ headers
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/pipenet_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/pipe_network_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/pipe_network.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/pipe_network.fbs
    COMMENT "Generating FlatBuffers headers for pipe_network"
)

add_custom_target(pipenet_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/pipe_network_generated.h
)

add_executable(pipe_networkd
    main.cpp
    PipeNetworkService.cpp
    PipeNetwork.cpp
    CableGraph.cpp
    Client/MessageRouterClient.cpp
)

target_include_directories(pipe_networkd PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(pipe_networkd pipenet_fbs)

target_link_libraries(pipe_networkd PRIVATE
    flatbuffers::flatbuffers
    spdlog::spdlog
    asio::asio
    Threads::Threads
)

# Unit tests
add_executable(pipe_network_test
    pipe_network_test.cpp
    PipeNetwork.cpp
    CableGraph.cpp
    FluidRegistry.cpp
)

target_include_directories(pipe_network_test PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(pipe_network_test pipenet_fbs)

target_link_libraries(pipe_network_test PRIVATE
    flatbuffers::flatbuffers
    spdlog::spdlog
    asio::asio
    Threads::Threads
)

add_test(NAME pipe_network_test COMMAND pipe_network_test)
