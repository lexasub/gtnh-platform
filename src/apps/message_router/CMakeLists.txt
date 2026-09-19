cmake_minimum_required(VERSION 3.20)

# MessageRouter — Go pub/sub service
# Build, run, debug via CMake + Go toolchain

# Go toolchain (v22+)
set(GO_MODULE "github.com/gtnh/platform/message_router")
set(GO_TARGET "routerd")
set(GO_BIN "/usr/bin/go")

# Note: Go compilation happens via custom target (go_build_${GO_TARGET}), not CMake.
# add_executable() provides the binary target for install/run/debug targets.
# Dummy source to satisfy CMake — Go binary built via custom target only
add_executable(${GO_TARGET} dummy.cpp)

# Go build
add_custom_target(go_build_${GO_TARGET}
    COMMAND ${GO_BIN} build -o ${CMAKE_CURRENT_BINARY_DIR}/${GO_TARGET}
        .
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/go.mod
        ${CMAKE_CURRENT_SOURCE_DIR}/go.sum
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building ${GO_TARGET}"
    VERBATIM
)

# Install (copy binary to build/install target)
add_custom_target(install_${GO_TARGET}
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_BINARY_DIR}/${GO_TARGET}
        ${CMAKE_INSTALL_PREFIX}/bin/${GO_TARGET}
    DEPENDS go_build_${GO_TARGET}
    COMMENT "Installing ${GO_TARGET}"
)

# Debug target (race detector)
add_custom_target(debug_${GO_TARGET}
    COMMAND ${GO_BIN} run -race ./${GO_MODULE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running ${GO_TARGET} in debug mode (with race detector)"
    VERBATIM
)

# Run target (build first, then exec)
add_custom_target(run_${GO_TARGET}
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${GO_TARGET}
    DEPENDS go_build_${GO_TARGET}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running ${GO_TARGET}"
    VERBATIM
)

# Convenience alias: build → run
add_custom_target(message_router
    COMMAND ${CMAKE_COMMAND} -P - <<'EOF'
set(targets "go_build_${GO_TARGET}" "run_message_router")
foreach(t ${targets})
    execute_process(COMMAND ${CMAKE_COMMAND} --build . --target ${t}
        RESULT_VARIABLE rc
        ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Failed to ${t}: ${err}")
endif()
endforeach()
EOF
    COMMENT "Build and run message_router"
)

# Run with default TCP port (4000)
add_custom_target(run_message_router
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${GO_TARGET} -port=4000
    DEPENDS go_build_${GO_TARGET}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Run message_router on TCP port 4000"
    VERBATIM
)

# Run with custom port
add_custom_target(run_message_router_custom
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${GO_TARGET} -port=1234
    DEPENDS go_build_${GO_TARGET}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Run message_router on TCP port 1234"
    VERBATIM
)
