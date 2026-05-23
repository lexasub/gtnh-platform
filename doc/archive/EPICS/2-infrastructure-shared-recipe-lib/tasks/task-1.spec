{
  "title": "Phase 1: Extract — создать библиотеку recipe_manager_lib",
  "description": "Создать src/libs/recipe_manager_lib/ как статическую библиотеку. Скопировать из simulation_core/RecipeManager/ все общие файлы (RecipeManager, ConditionEvaluator, ItemRegistry, RecipeConditions). Написать CMakeLists.txt с зависимостями (EnTT, nlohmann_json, spdlog, FlatBuffers). Библиотека должна компилироваться независимо.",
  "ecs_components": ["MachineComponent", "EnergyStorage", "Block"],
  "flatbuffers_schemas": ["src/protocol/core.fbs: ItemStack, Container", "src/protocol/recipe.fbs: MachineType, RecipeFrame"],
  "service_architecture": "STATIC library — target_link_libraries() без отдельного процесса.",
  "inputs": {
    "from": "src/services/simulation_core/RecipeManager/",
    "to": "src/libs/recipe_manager_lib/"
  },
  "constraints": [
    "Ничего не менять в API классов — только перемещение файлов",
    "Include guards остаются без изменений",
    "FlatBuffers генерация — в каждом сервисе своя (FBS_GEN_DIR), библиотека использует сгенерированные хедеры от сервиса через target_include_directories",
    "ItemRegistry остаётся синглтоном"
  ],
  "test_requirements": "Библиотека компилируется standalone. Все public методы RecipeManager доступны линковщику."
}
