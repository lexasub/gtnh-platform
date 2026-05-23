add_executable(gtnh_net_test
    test_main.cpp
    test_frame.cpp
    test_context.cpp
    test_echo.cpp
    test_toctou.cpp
)

target_link_libraries(gtnh_net_test PRIVATE
    gtnh-net
    pthread
)

# Standalone TOCTOU test (echo test has pre-existing segfault)
add_executable(toctou_test
    toctou_standalone.cpp
)

target_link_libraries(toctou_test PRIVATE
    gtnh-net
    pthread
)

# Integration test: server (IoUringConnection send) + client (IoUringConnection recv)
add_executable(integration_test
    test_integration.cpp
)

target_link_libraries(integration_test PRIVATE
    gtnh-net
    pthread
)

# Register with CTest
include(CTest)
add_test(NAME gtnh_net_test COMMAND gtnh_net_test)
add_test(NAME toctou_test COMMAND toctou_test)
add_test(NAME integration_test COMMAND integration_test)
