add_library(gtnh_game_world STATIC
  BlockDrops.cpp
  BlockTransforms.cpp
  ChunkEventHandler.cpp
  ChunkSnapshot.cpp
  WorldContainerInventory.cpp
)
target_include_directories(gtnh_game_world PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/apps/simcore
  ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)
add_dependencies(gtnh_game_world simcored_fbs)
target_link_libraries(gtnh_game_world PUBLIC
  gtnh_engine_registry
  gtnh_wire
  gtnh-net
  EnTT::EnTT
  flatbuffers::flatbuffers
  spdlog::spdlog
)

target_link_libraries(gtnh_game_world PRIVATE
  asio::asio
)
