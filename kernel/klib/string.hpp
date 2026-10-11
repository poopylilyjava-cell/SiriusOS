#pragma once
#include <klib/types.hpp>

// GCC emits calls to these even in a freestanding build (struct copies, array
// initialisation), so the kernel must define them itself.
extern "C" {
void*  memset(void* dest, int value, usize count);
void*  memcpy(void* dest, const void* src, usize count);
void*  memmove(void* dest, const void* src, usize count);
int    memcmp(const void* a, const void* b, usize count);
usize  strlen(const char* s);
int    strncmp(const char* a, const char* b, usize count);
int    strcmp(const char* a, const char* b);
}

// Copy a NUL-terminated string into a buffer of `size` bytes, truncating if
// needed. Always terminates (when size > 0). Returns the source length, so a
// result >= size means the copy was truncated.
usize str_copy(char* dest, usize size, const char* src);

// Substring search, used for boot cmdline parsing.
bool str_contains(const char* haystack, const char* needle);
