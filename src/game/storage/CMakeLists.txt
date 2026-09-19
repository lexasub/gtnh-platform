add_library(gtnh_game_storage STATIC
    ChunkStoreRepository.cpp
    PlayerInventoryStore.cpp
    InventoryLoadHandler.cpp
    InventoryActionHandler.cpp
    ChestStateManager.cpp
    PlayerJoinedHandler.cpp
)

target_include_directories(gtnh_game_storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)

add_dependencies(gtnh_game_storage simcored_fbs)

target_link_libraries(gtnh_game_storage PRIVATE
    asio::asio
)

target_link_libraries(gtnh_game_storage PUBLIC
    gtnh_engine_sim
    gtnh_wire
    gtnh-net
    EnTT::EnTT
    spdlog::spdlog
    flatbuffers::flatbuffers
)
