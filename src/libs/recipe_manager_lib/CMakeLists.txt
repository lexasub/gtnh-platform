cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(flatbuffers REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(SQLite3 REQUIRED)

set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
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
    COMMENT "Generating FlatBuffers headers for recipe_manager_lib"
)

add_custom_target(recipe_lib_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
)

add_library(recipe_manager_lib STATIC
    RecipeManager.cpp
    ConditionEvaluator.cpp
    ItemRegistry.cpp
)

target_include_directories(recipe_manager_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
)

add_dependencies(recipe_manager_lib recipe_lib_fbs)

target_link_libraries(recipe_manager_lib PUBLIC
    flatbuffers::flatbuffers
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    SQLite::SQLite3
)
