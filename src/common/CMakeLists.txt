# Wire-protocol headers shared across apps and game layers (no compiled
# sources): GatewayMsg wire constants, ResourcePort*, SlotContainer,
# ResourceBufferStateCodec. Consumers link gtnh_wire instead of relying on
# the blanket ${CMAKE_SOURCE_DIR}/src include dir.
add_library(gtnh_wire INTERFACE)

target_include_directories(gtnh_wire INTERFACE
    ${CMAKE_SOURCE_DIR}/src
)
