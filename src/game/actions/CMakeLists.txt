# Player actions — break/place/interact handlers, wrench + drill tools.
# Extracted from simcore (game rules depend on engine, not vice versa).
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gtnh_game_actions STATIC
    ActionContext.cpp
    ActionDispatcher.cpp
    CasRunner.cpp
    PlayerActionDispatcher.cpp
    SetBlockCASHandler.cpp
    MachineSlotHandler.cpp
    handlers/BreakBlockHandler.cpp
    handlers/ChestInteractHandler.cpp
    handlers/MachineInteractHandler.cpp
    handlers/PlaceBlockHandler.cpp
    handTool/WrenchHandler.cpp
    handTool/ElectricDrillHandler.cpp
    handTool/ToolActionHandler.cpp
    handTool/WrenchActionHandler.cpp
    handTool/PipeWrenchResponseHandler.cpp
)

# Quoted includes reach simcore internals (Network/, Storage/, Common/) and
# simcore's generated FlatBuffers headers — same explicit pattern as
# gtnh_game_machines.
target_include_directories(gtnh_game_actions PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/apps/simcore
    ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)

add_dependencies(gtnh_game_actions simcored_fbs)

target_link_libraries(gtnh_game_actions PUBLIC
    gtnh_wire
    gtnh_engine_registry
    gtnh_engine_sim
    gtnh_game_quests
    gtnh-net
    EnTT::EnTT
)

target_link_libraries(gtnh_game_actions PRIVATE
    spdlog::spdlog
    asio::asio
)
