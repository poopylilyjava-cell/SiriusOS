# siriusd: the Dear ImGui desktop. backend/ holds the two files that are the
# whole of the Dear ImGui port; shell/ what is drawn with them.

include("${CMAKE_SOURCE_DIR}/../cmake/imgui.cmake")

# The desktop proper -- the shell and the applications inside it -- as a list
# the host harness builds too.
include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")

sirius_program(siriusd main.cpp ${SIRIUSD_DESKTOP_SOURCES})
target_include_directories(siriusd.elf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(siriusd.elf PRIVATE sirius_imgui_backends sirius)

# The Wayland compositor, if the wayland port is in the sysroot.
include("${CMAKE_CURRENT_SOURCE_DIR}/wayland/wayland.cmake")
sirius_add_compositor(siriusd.elf)
