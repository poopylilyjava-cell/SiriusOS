# The host harness: siriusd's Dear ImGui backends and desktop, running on the
# build machine. This is where the rasterizer and the platform backend are
# developed and debugged (ADR 0003) -- with a host debugger and sanitizers,
# instead of through a serial port.
#
#   cmake -S tests/host_harness -B build/host_harness -G Ninja && cmake --build build/host_harness
#   build/host_harness/siriusd_host                      an SDL window
#   build/host_harness/siriusd_host --headless 200       no window: time 200 frames
#   build/host_harness/siriusd_host --headless 5 --out frame.ppm --size 1920x1080
#   build/host_harness/siriusd_host --headless 40 --demo --out frame.ppm
#                                     ...with every application opened first
#   build/host_harness/siriusd_host --headless 150 --at "2:launch:Wayland Terminal" \
#       --at "60:click:500,300" --at "80:type:ls\n" --out frame.ppm
#                                     ...a script: Wayland clients from $PATH,
#                                     clicks and typing at the frames given
#   build/host_harness/siriusd_host --headless 60 --splash --frame-ms 16 --out frame.ppm
#                                     ...the loading screen, a second in (a
#                                     window always starts with it)
#
# The applications are the real ones, so the terminal runs this machine's
# /bin/sh and the file manager shows this machine's files.
#
# SDL2 is only needed for the window; without it the harness is headless.

cmake_minimum_required(VERSION 3.25)
project(sirius_host_harness LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/imgui.cmake")

include("${SIRIUS_REPO_ROOT}/user/siriusd/sources.cmake")
add_subdirectory("${SIRIUS_REPO_ROOT}/user/libsirius" libsirius)

add_executable(siriusd_host main.cpp ${SIRIUSD_DESKTOP_SOURCES})
target_include_directories(siriusd_host PRIVATE "${SIRIUS_REPO_ROOT}/user/siriusd/shell")
target_compile_options(siriusd_host PRIVATE -Wall -Wextra)
target_link_libraries(siriusd_host PRIVATE sirius_imgui_backends sirius)

# The compositor, against this machine's libwayland-server: Wayland clients
# built for Linux run in the harness's desktop.
include("${SIRIUS_REPO_ROOT}/user/siriusd/wayland/wayland.cmake")
sirius_add_compositor(siriusd_host)

find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_compile_definitions(siriusd_host PRIVATE HARNESS_HAVE_SDL)
    target_link_libraries(siriusd_host PRIVATE SDL2::SDL2)
else()
    message(STATUS "host harness: SDL2 not found -- building it headless only")
endif()
