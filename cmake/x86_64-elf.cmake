# CMake toolchain file for the freestanding Sirius kernel.
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-elf.cmake

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(SIRIUS_TARGET_TRIPLE x86_64-elf CACHE STRING "bare-metal target triple")

get_filename_component(_sirius_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SIRIUS_TOOLCHAIN_BIN "${_sirius_root}/toolchain/local/bin")

find_program(CMAKE_C_COMPILER   ${SIRIUS_TARGET_TRIPLE}-gcc     HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_CXX_COMPILER ${SIRIUS_TARGET_TRIPLE}-g++     HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_ASM_COMPILER ${SIRIUS_TARGET_TRIPLE}-gcc     HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_AR           ${SIRIUS_TARGET_TRIPLE}-ar      HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_RANLIB       ${SIRIUS_TARGET_TRIPLE}-ranlib  HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_OBJCOPY      ${SIRIUS_TARGET_TRIPLE}-objcopy HINTS ${SIRIUS_TOOLCHAIN_BIN})
find_program(CMAKE_OBJDUMP      ${SIRIUS_TARGET_TRIPLE}-objdump HINTS ${SIRIUS_TOOLCHAIN_BIN})

if(NOT CMAKE_C_COMPILER)
    message(FATAL_ERROR
        "${SIRIUS_TARGET_TRIPLE}-gcc not found.\n"
        "Build the cross toolchain first:  ./toolchain/build-cross.sh")
endif()

# There is no libc to link against, so CMake's compiler probe must not try.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
