// The memory functions GCC may call on its own -- for struct copies, array
// initialisation and loops it recognises -- even in a freestanding program.
#include <stddef.h>

extern "C" {

void* memset(void* dest, int value, size_t count) {
    auto* p = static_cast<unsigned char*>(dest);
    for (size_t i = 0; i < count; i++) p[i] = static_cast<unsigned char>(value);
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < count; i++) d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    if (d < s) { for (size_t i = 0; i < count; i++) d[i] = s[i]; }
    else       { for (size_t i = count; i > 0; i--) d[i - 1] = s[i - 1]; }
    return dest;
}

int memcmp(const void* a, const void* b, size_t count) {
    const auto* x = static_cast<const unsigned char*>(a);
    const auto* y = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < count; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

}
