cmake_minimum_required(VERSION 3.25)

project(sirius
    VERSION 0.0.1
    DESCRIPTION "Sirius OS -- an x86_64 kernel with a Dear ImGui desktop"
    LANGUAGES C CXX ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
    message(FATAL_ERROR
        "The kernel must be cross-compiled. Configure with:\n"
        "  cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-elf.cmake\n"
        "or just run: make kernel")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

add_subdirectory(kernel)
# The bring-up programs only: they use the kernel's compiler and no C library.
# The rest of user/ is a project of its own, built by x86_64-sirius-gcc.
add_subdirectory(user/bringup)
