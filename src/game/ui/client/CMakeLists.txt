
add_library(UI

    core/ActionRegistry.cpp
    core/ActionHandler.cpp
    core/InputBinder.cpp
    core/DragManager.cpp

    components/SlotGrid.cpp
    components/CraftingGrid.cpp
    components/ItemIndex.cpp

    player/ClientCraftingWindow.cpp
    block/MachineWindow.cpp
    block/ChestWindow.cpp
    player/RecipeInspectWindow.cpp
    player/PlayerInventory.cpp
    player/CreativeMenu.cpp
    player/QuestBookWindow.cpp
    player/ConsoleWindow.cpp
    player/GameScenario.cpp
    panels/NeiPanel.cpp

    Window.cpp
    InputManager.cpp
    UIManager.cpp
    UIDefaults.cpp
    BlockUIFactory.cpp
)

target_include_directories(UI PRIVATE ${CMAKE_SOURCE_DIR}/src/apps/game_client ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/libs)
target_compile_definitions(UI PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data")
target_link_libraries(UI PRIVATE
        renderlib
        gtnh_game_machines
        gtnh_game_quests)