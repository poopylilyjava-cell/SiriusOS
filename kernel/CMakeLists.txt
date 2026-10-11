# The Sirius kernel: freestanding C++20, higher-half, Limine boot protocol.

file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.S")

if(NOT KERNEL_SOURCES)
    message(STATUS "kernel: no sources yet -- skipping (this is expected before M0)")
    return()
endif()

add_executable(sirius.elf ${KERNEL_SOURCES})

target_include_directories(sirius.elf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/limine-protocol/include")

# Freestanding: no libc, no hosted assumptions, no runtime we did not write.
target_compile_options(sirius.elf PRIVATE
    # Debug info costs nothing at runtime -- the bootloader loads only PT_LOAD
    # segments -- and it is what turns a panic backtrace into file:line.
    -g
    -ffreestanding
    -fno-stack-protector
    -fno-stack-check
    -fno-omit-frame-pointer       # keep frame pointers: stack traces on panic
    -fno-PIC
    -Wall -Wextra
    # The kernel may not touch the red zone or the FPU/SSE: an interrupt can
    # arrive at any instruction and we do not save that state on kernel entry.
    -mno-red-zone
    -mno-80387 -mno-mmx -mno-sse -mno-sse2
    -mcmodel=kernel
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit>)

set(KERNEL_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/linker.ld")

target_link_options(sirius.elf PRIVATE
    # Multilib selection happens at link time: without these the driver resolves
    # -lgcc to the default libgcc, which was built *with* the red zone.
    -mno-red-zone
    -mcmodel=kernel
    -nostdlib
    -static
    -Wl,--build-id=none
    -Wl,-z,noexecstack
    -Wl,-z,max-page-size=0x1000
    -Wl,-T,${KERNEL_LINKER_SCRIPT}
    -Wl,-Map,${CMAKE_BINARY_DIR}/kernel/sirius.map)

# libgcc supplies 64-bit division and friends -- but only the red-zone-free build.
target_link_libraries(sirius.elf PRIVATE gcc)

set_target_properties(sirius.elf PROPERTIES LINK_DEPENDS ${KERNEL_LINKER_SCRIPT})
