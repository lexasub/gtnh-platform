# Wire-protocol headers shared across apps and game layers (no compiled
# sources): GatewayMsg wire constants, ResourcePort*, SlotContainer,
# ResourceBufferStateCodec, FlatBuffersVerify. Consumers link gtnh_wire instead
# of relying on the blanket ${CMAKE_SOURCE_DIR}/src include dir.
add_library(gtnh_wire INTERFACE)

target_include_directories(gtnh_wire INTERFACE
    ${CMAKE_SOURCE_DIR}/src
)

# gtnh_wire is an INTERFACE target with no compiled sources, so nothing
# generates the FlatBuffers headers it parses. FlatBuffersVerify.h itself is
# schema-agnostic (it is a template over the root type), so it compiles against
# the flatbuffers package alone; only its TEST needs a real message type, and
# that test generates core.fbs here rather than depending on whichever service
# happens to have generated it first. Deliberately not guarded behind a
# find_package(GTest) — the repo has zero GoogleTest usage and CI does not
# install it, so a guarded test silently vanishes from CI instead of failing it.
if(BUILD_TESTING)
    find_package(flatbuffers REQUIRED)

    set(WIRE_FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
    set(WIRE_FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${WIRE_FBS_GEN_DIR})

    add_custom_command(
        OUTPUT
            ${WIRE_FBS_GEN_DIR}/core_generated.h
        COMMAND flatbuffers::flatc
            --gen-mutable
            -I ${WIRE_FBS_DIR}
            -o ${WIRE_FBS_GEN_DIR}
            -c ${WIRE_FBS_DIR}/core.fbs
        DEPENDS flatbuffers::flatc
                ${WIRE_FBS_DIR}/core.fbs
        COMMENT "Generating FlatBuffers headers for gtnh_wire tests"
    )

    add_custom_target(gtnh_wire_fbs
        DEPENDS ${WIRE_FBS_GEN_DIR}/core_generated.h
    )

    add_executable(flatbuffers_verify_test
        test/FlatBuffersVerify_test.cpp
    )
    target_include_directories(flatbuffers_verify_test PRIVATE
        ${WIRE_FBS_GEN_DIR}
        ${flatbuffers_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(flatbuffers_verify_test PRIVATE
        gtnh_wire
        flatbuffers::flatbuffers
    )
    add_dependencies(flatbuffers_verify_test gtnh_wire_fbs)
    # test_GetRootAloneDoesNotValidate deliberately hands a 3-byte buffer to
    # flatbuffers::GetRoot to PROVE that GetRoot manufactures a non-null Table*
    # from bytes that are not a FlatBuffer. That is undefined behaviour inside
    # the third-party header (buffer.h reads a uoffset_t out of a 3-byte array),
    # so -Warray-bounds fires there. The UB is the thing under test, in someone
    # else's header, and the alternative would be to stop asserting the property
    # that makes every VerifyAndGetRoot call in production necessary. Scoped to
    # this one diagnostic and this one target; every other -Werror stays on.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(flatbuffers_verify_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Wno-error=array-bounds>)
    endif()
    add_test(NAME flatbuffers_verify_test COMMAND flatbuffers_verify_test)
endif()
