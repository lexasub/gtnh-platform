set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gtnh_engine_registry STATIC Registry.cpp)
# Consumers write <engine/registry/ItemId.h>; that resolves via the
# ${CMAKE_SOURCE_DIR}/src include dir already present in all services.
target_include_directories(gtnh_engine_registry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(BUILD_TESTING)
    add_executable(registry_test test/Registry_test.cpp)
    target_compile_definitions(registry_test PRIVATE
        REGISTRY_DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data/registry"
    )
    target_include_directories(registry_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(registry_test PRIVATE gtnh_engine_registry)
    add_test(NAME registry_test COMMAND registry_test)

    add_executable(openhashmap_test test/OpenHashMap_test.cpp)
    target_include_directories(openhashmap_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(openhashmap_test PRIVATE gtnh_engine_registry)
    add_test(NAME openhashmap_test COMMAND openhashmap_test)
endif()
