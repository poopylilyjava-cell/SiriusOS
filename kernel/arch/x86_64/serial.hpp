#pragma once
#include <klib/types.hpp>

// 16550 UART on COM1. This is the kernel's first and most reliable output: it
// works before paging, before the framebuffer, and inside a fault handler.
bool serial_init();
void serial_write_char(char c);
void serial_write(const char* s);
