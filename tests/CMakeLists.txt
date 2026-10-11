# Host-side tests. Deliberately a separate CMake project from the kernel: these
# build with the *host* compiler so the portable parts of Sirius -- page-table
# math, allocators, VFS path resolution, the ImGui software rasterizer, Wayland
# protocol marshalling -- can be tested without booting anything.

cmake_minimum_required(VERSION 3.25)
project(sirius_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")

if(NOT TEST_SOURCES)
    message(STATUS "tests: none yet -- add them under tests/unit/")
    return()
endif()

# Everything is built with the sanitizers, Dear ImGui and its backends
# included: the rasterizer writes pixels by pointer arithmetic.
add_compile_options(-g -fsanitize=address,undefined)
add_link_options(-fsanitize=address,undefined)

# The tests of siriusd's Dear ImGui backends link the real thing.
set(IMGUI_TESTS softrast_test imgui_platform_test)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/imgui.cmake")

foreach(src ${TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../kernel"
        "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include")
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    if(name IN_LIST IMGUI_TESTS)
        target_link_libraries(${name} PRIVATE sirius_imgui_backends)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endforeach()
