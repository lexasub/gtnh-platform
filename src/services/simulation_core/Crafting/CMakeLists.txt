add_library(Crafting STATIC
    GridPatternMatcher.cpp
    WorkbenchStateManager.cpp
)

target_include_directories(Crafting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs
)

target_link_libraries(Crafting PUBLIC
    recipe_manager_lib
)

target_link_libraries(Crafting PRIVATE
    spdlog::spdlog
    asio::asio
)
