# Header-only common utilities and the strict shared registry loader.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gtnh_common STATIC Registry.cpp)
target_include_directories(gtnh_common PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_executable(openhashmap_test test/OpenHashMap_test.cpp)
target_include_directories(openhashmap_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME openhashmap_test COMMAND openhashmap_test)

add_executable(registry_test test/Registry_test.cpp)
target_link_libraries(registry_test PRIVATE gtnh_common)
target_include_directories(registry_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(registry_test PRIVATE REGISTRY_DATA_DIR="${CMAKE_SOURCE_DIR}/data/registry")
add_test(NAME registry_test COMMAND registry_test)
