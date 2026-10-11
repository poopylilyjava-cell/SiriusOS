#pragma once
#include <klib/types.hpp>

struct Registers;

// Dump a backtrace by walking the saved frame pointer chain. The kernel is
// built with -fno-omit-frame-pointer specifically so this works.
void print_backtrace(u64 rbp);

[[noreturn]] void panic(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void panic_with_registers(const Registers* regs, const char* reason);
