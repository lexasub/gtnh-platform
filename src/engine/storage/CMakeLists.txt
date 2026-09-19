cmake_minimum_required(VERSION 3.20)

add_library(gtnh_engine_storage INTERFACE)

# ${CMAKE_SOURCE_DIR}/src is required so consumers can write
# <engine/storage/IEntityStateStorage.h> (path resolved from src/).
target_include_directories(gtnh_engine_storage INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
