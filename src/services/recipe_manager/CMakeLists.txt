cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(flatbuffers REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(asio REQUIRED)
find_package(EnTT REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# FlatBuffers schema → C++ headers
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/reciped_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/recipe.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/recipe.fbs
    COMMENT "Generating FlatBuffers headers for recipe_manager"
)

add_custom_target(reciped_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
)

add_executable(reciped
    main.cpp
    RecipeManagerService.cpp
    Client/MessageRouterClient.cpp
)

target_include_directories(reciped PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/libs
    ${CMAKE_SOURCE_DIR}/src/services
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(reciped reciped_fbs)

target_compile_definitions(reciped PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(reciped PRIVATE
    recipe_manager_lib
    flatbuffers::flatbuffers
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    asio::asio
    Threads::Threads
    SQLite::SQLite3
)
