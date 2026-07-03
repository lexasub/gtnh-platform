
add_library(UI

    Core/ActionRegistry.cpp
    Core/ActionHandler.cpp
    Core/InputBinder.cpp
    Core/DragManager.cpp

    Components/SlotGrid.cpp
    Components/CraftingGrid.cpp
    Components/ItemIndex.cpp

    Windows/player/ClientCraftingWindow.cpp
    Windows/block/MachineWindow.cpp
    Windows/block/ChestWindow.cpp
    Windows/player/RecipeInspectWindow.cpp
    Windows/player/PlayerInventory.cpp
    Windows/player/CreativeMenu.cpp
    Windows/player/QuestBookWindow.cpp

    Panels/NeiPanel.cpp

    Window.cpp
    InputManager.cpp
    UIManager.cpp
    UIDefaults.cpp
    BlockUIFactory.cpp
)

target_include_directories(UI PRIVATE ../Common . .. ${CMAKE_SOURCE_DIR}/src  ${CMAKE_SOURCE_DIR}/src/libs)
target_compile_definitions(UI PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_link_libraries(UI PRIVATE
        renderlib
        machine_registry
        quest_lib)