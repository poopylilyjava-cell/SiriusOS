# CMake toolchain file for Sirius userland: the hosted x86_64-sirius compiler,
# which finds mlibc and libstdc++ in /sysroot by itself.
#   cmake -S user -B build/userland -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-sirius.cmake

# CMake has no platform module for Sirius; Generic means "an ELF target, ask
# the compiler", which is all that is needed.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(SIRIUS_TARGET_TRIPLE x86_64-sirius CACHE STRING "userland target triple")

get_filename_component(_sirius_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SIRIUS_TOOLCHAIN_BIN "${_sirius_root}/toolchain/local/bin")

find_program(CMAKE_C_COMPILER   ${SIRIUS_TARGET_TRIPLE}-gcc     HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_CXX_COMPILER ${SIRIUS_TARGET_TRIPLE}-g++     HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_AR           ${SIRIUS_TARGET_TRIPLE}-ar      HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_RANLIB       ${SIRIUS_TARGET_TRIPLE}-ranlib  HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_STRIP        ${SIRIUS_TARGET_TRIPLE}-strip   HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_OBJCOPY      ${SIRIUS_TARGET_TRIPLE}-objcopy HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_OBJDUMP      ${SIRIUS_TARGET_TRIPLE}-objdump HINTS ${SIRIUS_TOOLCHAIN_BIN})

if(NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
    message(FATAL_ERROR
        "${SIRIUS_TARGET_TRIPLE}-gcc not found.\n"
        "Build the system toolchain first:  ./toolchain/build-sirius.sh")
endif()

# Libraries and headers come from the sysroot only, never from the host.
set(SIRIUS_SYSROOT "${_sirius_root}/sysroot")
set(CMAKE_FIND_ROOT_PATH "${SIRIUS_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
