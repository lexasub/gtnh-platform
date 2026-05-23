# FlatBuffers schema → C++ headers — shared protocol from src/protocol/
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gateway_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

set(CMAKE_CXX_STANDARD 26)
add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/gateway_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/gateway.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/gateway.fbs
    COMMENT "Generating FlatBuffers headers for gateway"
)

add_custom_target(gateway_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/gateway_generated.h
)

add_executable(gatewayd
    main.cpp
    gateway.cpp
)

add_dependencies(gatewayd gateway_fbs)
include_directories(BEFORE SYSTEM ${flatbuffers_INCLUDE_DIRS} ${FBS_GEN_DIR})

target_link_libraries(gatewayd PRIVATE
    gtnh-net
    spdlog::spdlog
    uring
    pthread
    flatbuffers::flatbuffers
)
