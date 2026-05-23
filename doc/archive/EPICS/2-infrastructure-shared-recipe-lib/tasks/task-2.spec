{
  "title": "Phase 2: SimulationCore — переключить на библиотеку",
  "description": "В CMakeLists.txt simulation_core: удалить RecipeManager/ из add_executable/srcs, добавить target_link_libraries(... recipe_manager_lib). Проверить include paths (добавить ${CMAKE_SOURCE_DIR}/src/libs/recipe_manager_lib). Убедиться что simulation_core собирается.",
  "ecs_components": [],
  "flatbuffers_schemas": [],
  "service_architecture": "SimulationCore линкует библиотеку вместо собственных .cpp",
  "constraints": [
    "include paths: ${CMAKE_SOURCE_DIR}/src/libs/recipe_manager_lib",
    "RecipeManager API остаётся тем же — код машинного тика не меняется",
    "FlatBuffers генерация остаётся в CMakeLists.txt simulation_core"
  ],
  "test_requirements": "SimulationCore компилируется. Машинный тик с evaluateConditions работает как раньше."
}
