# Bring-up user programs: static ELF executables built with the kernel's
# bare-metal x86_64-elf compiler and no C library, so that the kernel can be
# built and tested without the x86_64-sirius toolchain. They talk to the
# kernel through runtime.hpp alone, and live on the root filesystem under
# /bin (some test entries also load them as Limine boot modules). Everything
# else in user/ is built against mlibc, by user/CMakeLists.txt.

set(BRINGUP_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/user.ld")

# GCC recognises the loops in memset and friends and would compile them into
# calls to themselves.
set_source_files_properties(string.cpp PROPERTIES COMPILE_OPTIONS -fno-tree-loop-distribute-patterns)

function(sirius_bringup_program name)
    add_executable(${name}.elf crt0.S string.cpp ${ARGN})
    target_include_directories(${name}.elf PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/kernel/include")
    # Ring 3 may use the red zone and SSE: interrupts arrive on the kernel
    # stack, and the scheduler saves FPU state for user threads.
    target_compile_options(${name}.elf PRIVATE
        -O2 -g
        -ffreestanding -fno-stack-protector -fno-pic
        -Wall -Wextra
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
    target_link_options(${name}.elf PRIVATE
        -nostdlib -static
        -Wl,--build-id=none
        -Wl,-z,noexecstack
        -Wl,-z,max-page-size=0x1000
        -Wl,-T,${BRINGUP_LINKER_SCRIPT})
    target_link_libraries(${name}.elf PRIVATE gcc)
    set_target_properties(${name}.elf PROPERTIES
        LINK_DEPENDS ${BRINGUP_LINKER_SCRIPT}
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/user")
endfunction()

sirius_bringup_program(ticker ticker.cpp)
sirius_bringup_program(probe probe.cpp)
sirius_bringup_program(init init.cpp)
sirius_bringup_program(hello hello.cpp)
sirius_bringup_program(fstest fstest.cpp)
