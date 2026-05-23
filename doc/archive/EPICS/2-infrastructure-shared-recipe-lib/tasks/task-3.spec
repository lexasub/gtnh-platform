{
  "title": "Phase 2: recipe_manager — переключить на библиотеку",
  "description": "В CMakeLists.txt recipe_manager: удалить RecipeManager/ из add_executable/srcs, добавить target_link_libraries(... recipe_manager_lib). Проверить include paths. Убедиться что reciped собирается.",
  "ecs_components": [],
  "flatbuffers_schemas": [],
  "service_architecture": "recipe_manager (reciped) линкует библиотеку вместо собственных .cpp",
  "constraints": [
    "include paths: ${CMAKE_SOURCE_DIR}/src/libs/recipe_manager_lib",
    "RecipeManagerService.cpp не меняется — он вызывает тот же API"
  ],
  "test_requirements": "reciped компилируется. RPC обработчики работают как раньше."
}
