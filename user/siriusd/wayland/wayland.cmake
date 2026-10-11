# The Wayland compositor's part of siriusd (wayland/), for whichever project
# builds the desktop: user/siriusd for Sirius, tests/host_harness for the
# build machine.
#
#   include(<repo>/user/siriusd/wayland/wayland.cmake)
#   sirius_add_compositor(target)
#
# Adds the compositor to `target` and defines SIRIUSD_WAYLAND, if
# libwayland-server is there to build it with: in the sysroot for Sirius
# (./tools/ports.sh add libffi wayland), from pkg-config on the build
# machine. Without it the desktop is built without a compositor, and says so.
#
# The protocol code is generated from the XML in protocols/ by
# wayland-scanner, which runs on the build machine in either case.

set(SIRIUSD_WAYLAND_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(sirius_add_compositor target)
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(SIRIUS_SYSROOT)
        # CMake's Generic platform has no system library directories to look
        # in: name the sysroot's.
        find_library(WAYLAND_SERVER_LIBRARY wayland-server PATHS "${SIRIUS_SYSROOT}/usr/lib" NO_CMAKE_FIND_ROOT_PATH NO_DEFAULT_PATH)
        find_library(FFI_LIBRARY ffi PATHS "${SIRIUS_SYSROOT}/usr/lib" NO_CMAKE_FIND_ROOT_PATH NO_DEFAULT_PATH)
        if(WAYLAND_SERVER_LIBRARY AND FFI_LIBRARY)
            set(libraries ${WAYLAND_SERVER_LIBRARY} ${FFI_LIBRARY})
        endif()
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
            if(WAYLAND_SERVER_FOUND)
                set(libraries PkgConfig::WAYLAND_SERVER)
            endif()
        endif()
    endif()
    if(NOT WAYLAND_SCANNER OR NOT libraries)
        message(STATUS "siriusd: no libwayland-server or wayland-scanner -- building the desktop without a compositor")
        return()
    endif()

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/wayland-generated")
    file(MAKE_DIRECTORY "${generated}")
    set(sources)
    foreach(protocol xdg-shell xdg-decoration-unstable-v1)
        set(xml "${SIRIUSD_WAYLAND_DIR}/protocols/${protocol}.xml")
        add_custom_command(
            OUTPUT "${generated}/${protocol}-server-protocol.h" "${generated}/${protocol}-protocol.c"
            COMMAND ${WAYLAND_SCANNER} server-header "${xml}" "${generated}/${protocol}-server-protocol.h"
            COMMAND ${WAYLAND_SCANNER} private-code "${xml}" "${generated}/${protocol}-protocol.c"
            DEPENDS "${xml}"
            COMMENT "wayland-scanner: ${protocol}")
        list(APPEND sources "${generated}/${protocol}-server-protocol.h" "${generated}/${protocol}-protocol.c")
    endforeach()
    foreach(layout us de)
        add_custom_command(
            OUTPUT "${generated}/keymap-${layout}.c"
            COMMAND ${CMAKE_COMMAND} -DINPUT=${SIRIUSD_WAYLAND_DIR}/keymaps/${layout}.xkb
                    -DOUTPUT=${generated}/keymap-${layout}.c -DNAME=sirius_keymap_${layout}
                    -P "${SIRIUSD_WAYLAND_DIR}/embed.cmake"
            DEPENDS "${SIRIUSD_WAYLAND_DIR}/keymaps/${layout}.xkb" "${SIRIUSD_WAYLAND_DIR}/embed.cmake"
            COMMENT "embedding the ${layout} keymap")
        list(APPEND sources "${generated}/keymap-${layout}.c")
    endforeach()

    target_sources(${target} PRIVATE
        "${SIRIUSD_WAYLAND_DIR}/compositor.cpp"
        "${SIRIUSD_WAYLAND_DIR}/surface.cpp"
        "${SIRIUSD_WAYLAND_DIR}/xdg.cpp"
        "${SIRIUSD_WAYLAND_DIR}/seat.cpp"
        "${SIRIUSD_WAYLAND_DIR}/data.cpp"
        "${SIRIUSD_WAYLAND_DIR}/window.cpp"
        ${sources})
    target_include_directories(${target} PRIVATE "${generated}")
    target_compile_definitions(${target} PRIVATE SIRIUSD_WAYLAND)
    target_link_libraries(${target} PRIVATE ${libraries})
    message(STATUS "siriusd: with the Wayland compositor")
endfunction()
