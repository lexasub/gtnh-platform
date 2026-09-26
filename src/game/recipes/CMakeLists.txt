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
    COMMENT "Generating FlatBuffers headers for gtnh_game_recipes"
)

add_custom_target(recipe_lib_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
)

add_library(gtnh_game_recipes STATIC
    RecipeManager.cpp
    ConditionEvaluator.cpp
    ItemRegistry.cpp
)

target_include_directories(gtnh_game_recipes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
)

add_dependencies(gtnh_game_recipes recipe_lib_fbs)

target_link_libraries(gtnh_game_recipes PUBLIC
    gtnh_engine_registry
    flatbuffers::flatbuffers
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    SQLite::SQLite3
)

# ConditionEvaluator unit tests — the per-clause gate that decides whether a
# recipe may run against a MachineState. Self-contained: the evaluator is pure
# in-memory logic over RecipeConditions, so the test links gtnh_game_recipes
# and needs no services, no data files and no wall clock.
if(BUILD_TESTING)
    add_executable(condition_evaluator_test
        test/test_condition_evaluator.cpp
    )
    target_include_directories(condition_evaluator_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(condition_evaluator_test PRIVATE
        gtnh_game_recipes
    )
    add_test(NAME condition_evaluator_test COMMAND condition_evaluator_test)

    # recipe_manager_matching_test — gp-82b9 (findRecipeByInputs matching) and
    # the craft-boundary half of gp-1t0w (input consumption, one-shot output
    # production). Same CHECK/TEST harness, same no-GTest rule as above.
    #
    # It DOES read the real data files: the loader resolves `item:` strings and
    # the machine tier/energy maps come from items.csv, recipes/ and
    # machines.yaml, so a fixture-free manager cannot exercise the resolution
    # path these issues are about. DATA_DIR is an absolute path injected by
    # CMake, so nothing depends on the CWD. Recipe fixtures are written to
    # mkstemp files and removed again; no directory, network or wall clock.
    add_executable(recipe_manager_matching_test
        test/test_recipe_manager_matching.cpp
    )
    target_compile_definitions(recipe_manager_matching_test PRIVATE
        DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data"
    )
    target_include_directories(recipe_manager_matching_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(recipe_manager_matching_test PRIVATE
        gtnh_game_recipes
        flatbuffers::flatbuffers
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        SQLite::SQLite3
    )
    add_test(NAME recipe_manager_matching_test
             COMMAND recipe_manager_matching_test)
endif()
