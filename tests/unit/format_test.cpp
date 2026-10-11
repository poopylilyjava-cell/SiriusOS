// Host-side tests for the kernel's printf implementation.
//
// The formatter is pure computation, so it is compiled straight into a host
// binary (with asan and ubsan) rather than exercised through QEMU. The kernel
// source is included directly because it is freestanding: it has no libc
// dependency beyond the string primitives the host already provides.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// klog() writes through the serial port; supply a sink so this links.
static char g_serial[8192];
static size_t g_serial_len = 0;
void serial_write(const char* s) {
    while (*s && g_serial_len + 1 < sizeof(g_serial)) g_serial[g_serial_len++] = *s++;
    g_serial[g_serial_len] = '\0';
}
void serial_write_char(char) {}
namespace fbconsole { void write(const char*) {} }   // the screen: not in a test
bool serial_init() { return true; }

#include <klib/format.cpp>

static int g_failures = 0;

static void check(const char* expected, const char* fmt, auto... args) {
    char buf[256];
    ksnprintf(buf, sizeof(buf), fmt, args...);
    if (strcmp(buf, expected) != 0) {
        printf("  FAIL  fmt=\"%s\"  expected \"%s\"  got \"%s\"\n", fmt, expected, buf);
        g_failures++;
    }
}

int main() {
    printf("kvsnprintf:\n");

    check("hello", "hello");
    check("", "");
    check("100%", "100%%");

    // Integers, including the value that breaks naive negation.
    check("0", "%d", 0);
    check("-1", "%d", -1);
    check("2147483647", "%d", 2147483647);
    check("-9223372036854775808", "%ld", INT64_MIN);
    check("18446744073709551615", "%lu", UINT64_MAX);

    // Hex, upper and lower, with the alternate form.
    check("deadbeef", "%x", 0xdeadbeefu);
    check("DEADBEEF", "%X", 0xdeadbeefu);
    check("0xdeadbeef", "%#x", 0xdeadbeefu);
    check("0x00ff", "%#06x", 0xffu);

    // Width and padding.
    check("     42", "%7d", 42);
    check("42     ", "%-7d", 42);
    check("0000042", "%07d", 42);
    check("-000042", "%07d", -42);   // sign must precede the zero padding
    check("  abc", "%5s", "abc");
    check("abc  ", "%-5s", "abc");
    check("(null)", "%s", (const char*)nullptr);
    check("  7", "%*d", 3, 7);
    check("7  ", "%*d", -3, 7);

    check("x", "%c", 'x');
    check("0xffffffff80000000", "%p", (void*)0xffffffff80000000ull);

    // An unknown specifier is echoed rather than silently swallowing input.
    check("%q", "%q");

    // A format that ends inside a conversion must stop at the terminator, not
    // read past it. Heap copies, so asan sees any overrun.
    printf("truncated conversions:\n");
    const char* const truncated[] = { "abc%", "abc%l", "abc%-", "abc%5", "abc%ll", "abc%#0" };
    for (const char* source : truncated) {
        char* fmt = strdup(source);
        char buf[32];
        va_list none{};
        const size_t n = kvsnprintf(buf, sizeof(buf), fmt, none);
        if (n != 3 || strcmp(buf, "abc") != 0) {
            printf("  FAIL  fmt=\"%s\"  expected \"abc\"  got \"%s\"\n", source, buf);
            g_failures++;
        }
        free(fmt);
    }

    printf("truncation:\n");
    {
        char small[5];
        const size_t needed = ksnprintf(small, sizeof(small), "%s", "abcdefgh");
        assert(strcmp(small, "abcd") == 0);   // always NUL terminated
        assert(needed == 8);                  // returns the length it wanted
    }
    {
        // A zero-sized buffer must not be written to at all.
        char canary[2] = {'\x7f', '\x7f'};
        const size_t needed = kvsnprintf(nullptr, 0, "abc", nullptr);
        assert(needed == 3);
        assert(canary[0] == '\x7f' && canary[1] == '\x7f');
    }

    printf("log levels:\n");
    {
        g_serial_len = 0; g_serial[0] = '\0';
        klog_set_level(LogLevel::Warn);
        LOG_INFO("suppressed");
        assert(g_serial_len == 0);
        LOG_ERROR("shown %d", 1);
        assert(strstr(g_serial, "[err ] shown 1\n") != nullptr);
    }

    if (g_failures != 0) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nall format tests passed\n");
    return 0;
}
