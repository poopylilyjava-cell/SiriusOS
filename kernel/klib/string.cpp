#include <klib/string.hpp>

extern "C" {

void* memset(void* dest, int value, usize count) {
    auto* p = static_cast<u8*>(dest);
    for (usize i = 0; i < count; i++) p[i] = static_cast<u8>(value);
    return dest;
}

void* memcpy(void* dest, const void* src, usize count) {
    auto* d = static_cast<u8*>(dest);
    const auto* s = static_cast<const u8*>(src);
    for (usize i = 0; i < count; i++) d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, usize count) {
    auto* d = static_cast<u8*>(dest);
    const auto* s = static_cast<const u8*>(src);
    if (d == s || count == 0) return dest;
    // Overlapping and dest above src: copy backwards or we clobber the tail.
    if (d < s) {
        for (usize i = 0; i < count; i++) d[i] = s[i];
    } else {
        for (usize i = count; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void* a, const void* b, usize count) {
    const auto* x = static_cast<const u8*>(a);
    const auto* y = static_cast<const u8*>(b);
    for (usize i = 0; i < count; i++)
        if (x[i] != y[i]) return static_cast<int>(x[i]) - static_cast<int>(y[i]);
    return 0;
}

usize strlen(const char* s) {
    usize n = 0;
    while (s[n]) n++;
    return n;
}

int strncmp(const char* a, const char* b, usize count) {
    for (usize i = 0; i < count; i++) {
        if (a[i] != b[i]) return static_cast<int>(static_cast<u8>(a[i])) -
                                 static_cast<int>(static_cast<u8>(b[i]));
        if (a[i] == '\0') return 0;
    }
    return 0;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return static_cast<int>(static_cast<u8>(*a)) - static_cast<int>(static_cast<u8>(*b));
}

} // extern "C"

usize str_copy(char* dest, usize size, const char* src) {
    const usize length = strlen(src);
    if (size == 0) return length;
    const usize n = length < size - 1 ? length : size - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
    return length;
}

bool str_contains(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    const usize n = strlen(needle);
    if (n == 0) return true;
    for (usize i = 0; haystack[i]; i++)
        if (strncmp(&haystack[i], needle, n) == 0) return true;
    return false;
}
