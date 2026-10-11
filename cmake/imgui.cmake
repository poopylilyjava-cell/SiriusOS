# Dear ImGui and the two Sirius backends, as static libraries. Included by
# every project that draws with it: user/ (for Sirius), tests/host_harness and
# tests/ (for the build machine). Each project compiles its own copy, with
# its own compiler.
#
#   include(<repo>/cmake/imgui.cmake)
#   target_link_libraries(app PRIVATE sirius_imgui_backends)

get_filename_component(SIRIUS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(IMGUI_DIR "${SIRIUS_REPO_ROOT}/third_party/imgui")
set(SIRIUS_BACKEND_DIR "${SIRIUS_REPO_ROOT}/user/siriusd/backend")

if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui is missing: git submodule update --init third_party/imgui")
endif()

add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${SIRIUS_BACKEND_DIR}")
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="sirius_imconfig.h")
# Not our code: optimise it, even in a debug build, and do not lint it.
target_compile_options(imgui PRIVATE -O2 -w)

# The renderer and platform backends: the whole of the port (ADR 0003).
add_library(sirius_imgui_backends STATIC
    "${SIRIUS_BACKEND_DIR}/imgui_impl_softrast.cpp"
    "${SIRIUS_BACKEND_DIR}/imgui_impl_sirius.cpp")
target_include_directories(sirius_imgui_backends PUBLIC
    "${SIRIUS_BACKEND_DIR}"
    "${SIRIUS_REPO_ROOT}/kernel/include")
target_compile_options(sirius_imgui_backends PRIVATE -O2 -Wall -Wextra)
target_link_libraries(sirius_imgui_backends PUBLIC imgui)
