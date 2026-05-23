{
  "title": "Phase 3: Cleanup — удалить дубликаты",
  "description": "После успешной компиляции обоих сервисов: удалить src/services/simulation_core/RecipeManager/ и src/services/recipe_manager/RecipeManager/. Провести grep по всему проекту на предмет оставшихся include или ссылок на старые пути. Единственный источник — src/libs/recipe_manager_lib/.",
  "ecs_components": [],
  "flatbuffers_schemas": [],
  "service_architecture": "Чистка мусора после рефакторинга",
  "constraints": [
    "Все include пути должны указывать на src/libs/recipe_manager_lib/",
    "Удалить только файлы, которые были перемещены в библиотеку"
  ],
  "test_requirements": "Полный build всех сервисов проходит без ошибок. Сравнить бинарники до/после — функциональность идентична."
}
