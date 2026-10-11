# Sirius userland: hosted programs, built by the x86_64-sirius toolchain
# against mlibc and libstdc++.
#
# A project of its own, because one CMake build tree has one compiler and
# the kernel's is the bare-metal x86_64-elf:
#
#   cmake -S user -B build/userland -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-sirius.cmake
#   (or just: make userland)
#
# user/bringup/ is not part of it: those programs predate the C library and
# are built with the kernel, by the top-level project.

cmake_minimum_required(VERSION 3.25)

project(sirius_userland
    VERSION 0.0.1
    DESCRIPTION "Sirius OS -- user space"
    LANGUAGES C CXX)

if(NOT CMAKE_C_COMPILER MATCHES "x86_64-sirius")
    message(FATAL_ERROR
        "Userland must be built with the x86_64-sirius toolchain. Configure with:\n"
        "  cmake -S user -B build/userland -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-sirius.cmake\n"
        "or just run: make userland")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

# A program installed as /bin/<name>. tools/mkimage.sh picks up everything in
# the build tree's bin/ and strips it on the way onto the disk; the copy here
# keeps its debug information.
function(sirius_program name)
    add_executable(${name}.elf ${ARGN})
    target_compile_options(${name}.elf PRIVATE -O2 -g -Wall -Wextra)
    # Static archives carry all of libc and libstdc++; keep only what is used.
    target_compile_options(${name}.elf PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(${name}.elf PRIVATE -Wl,--gc-sections)
    # The C library is not a CMake target, but `make libc` replaces it: relink.
    set_target_properties(${name}.elf PROPERTIES
        LINK_DEPENDS "${SIRIUS_SYSROOT}/usr/lib/libc.a;${SIRIUS_SYSROOT}/usr/lib/crt1.o"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

add_subdirectory(libsirius)
add_subdirectory(tests)
add_subdirectory(apps)
add_subdirectory(siriusd)
