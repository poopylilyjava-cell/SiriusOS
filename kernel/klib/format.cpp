#include <drivers/fbconsole.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <arch/x86_64/serial.hpp>

namespace {

LogLevel g_level = LogLevel::Info;

// A bounded output cursor. Writing past the end is counted but discarded, so
// callers can size a buffer from the return value the way snprintf allows.
struct Sink {
    char* buf;
    usize cap;
    usize len = 0;

    void put(char c) {
        if (len + 1 < cap) buf[len] = c;
        len++;
    }
    void puts(const char* s) { while (*s) put(*s++); }
    void pad(char c, usize n) { for (usize i = 0; i < n; i++) put(c); }
};

constexpr const char* kDigitsLower = "0123456789abcdef";
constexpr const char* kDigitsUpper = "0123456789ABCDEF";

// Renders into a caller-owned scratch buffer and returns its start. 64-bit
// binary is the widest case at 64 digits, but we only emit base 10 and 16.
char* render_unsigned(u64 value, unsigned base, bool upper, char* end) {
    const char* digits = upper ? kDigitsUpper : kDigitsLower;
    char* p = end;
    *--p = '\0';
    if (value == 0) { *--p = '0'; return p; }
    while (value) {
        *--p = digits[value % base];
        value /= base;
    }
    return p;
}

void emit_padded(Sink& out, const char* text, usize width, bool left, bool zero) {
    const usize len = strlen(text);
    const usize padding = width > len ? width - len : 0;

    if (left) {
        out.puts(text);
        out.pad(' ', padding);
        return;
    }
    // Zero padding must follow a '-' sign, not precede it.
    if (zero && text[0] == '-') {
        out.put('-');
        out.pad('0', padding);
        out.puts(text + 1);
        return;
    }
    out.pad(zero ? '0' : ' ', padding);
    out.puts(text);
}

} // namespace

void klog_set_level(LogLevel level) { g_level = level; }
bool klog_enabled(LogLevel level)   { return static_cast<u8>(level) <= static_cast<u8>(g_level); }

usize kvsnprintf(char* buf, usize size, const char* fmt, va_list args) {
    Sink out{buf, size};

    for (const char* f = fmt; *f; f++) {
        if (*f != '%') { out.put(*f); continue; }
        f++;
        if (*f == '\0') break;

        bool left = false, zero = false, alt = false;
        for (;; f++) {
            if (*f == '-')      left = true;
            else if (*f == '0') zero = true;
            else if (*f == '#') alt = true;
            else break;
        }

        usize width = 0;
        if (*f == '*') {
            const int w = va_arg(args, int);
            if (w < 0) { left = true; width = static_cast<usize>(-w); }
            else width = static_cast<usize>(w);
            f++;
        } else {
            while (*f >= '0' && *f <= '9') width = width * 10 + static_cast<usize>(*f++ - '0');
        }

        // Length modifiers: we only need to know "is it 64-bit".
        bool wide = false;
        if (*f == 'z') { wide = true; f++; }
        else if (*f == 'l') { wide = true; f++; if (*f == 'l') f++; }
        else if (*f == 'h') { f++; if (*f == 'h') f++; }

        // The string ended inside a conversion ("%l", "%-5"). Stop here: the
        // outer loop's f++ would otherwise step over the terminator.
        if (*f == '\0') break;

        char scratch[24];
        char* end = scratch + sizeof(scratch);

        switch (*f) {
        case 'c': {
            char s[2] = { static_cast<char>(va_arg(args, int)), '\0' };
            emit_padded(out, s, width, left, false);
            break;
        }
        case 's': {
            const char* s = va_arg(args, const char*);
            emit_padded(out, s ? s : "(null)", width, left, false);
            break;
        }
        case 'd':
        case 'i': {
            const i64 v = wide ? va_arg(args, i64) : static_cast<i64>(va_arg(args, i32));
            // Negating INT64_MIN overflows, so build the magnitude unsigned.
            const u64 magnitude = v < 0 ? (~static_cast<u64>(v) + 1) : static_cast<u64>(v);
            char* p = render_unsigned(magnitude, 10, false, end);
            if (v < 0) *--p = '-';
            emit_padded(out, p, width, left, zero);
            break;
        }
        case 'u': {
            const u64 v = wide ? va_arg(args, u64) : static_cast<u64>(va_arg(args, u32));
            emit_padded(out, render_unsigned(v, 10, false, end), width, left, zero);
            break;
        }
        case 'x':
        case 'X': {
            const u64 v = wide ? va_arg(args, u64) : static_cast<u64>(va_arg(args, u32));
            char* p = render_unsigned(v, 16, *f == 'X', end);
            if (alt) {
                // The 0x prefix is part of the field, so it consumes width.
                out.puts(*f == 'X' ? "0X" : "0x");
                if (width > 2) width -= 2; else width = 0;
            }
            emit_padded(out, p, width, left, zero);
            break;
        }
        case 'p': {
            const auto v = reinterpret_cast<u64>(va_arg(args, void*));
            char* p = render_unsigned(v, 16, false, end);
            out.puts("0x");
            emit_padded(out, p, width > 2 ? width - 2 : 0, left, true);
            break;
        }
        case '%':
            out.put('%');
            break;
        default:
            // Unknown specifier: emit it literally rather than silently eating it.
            out.put('%');
            out.put(*f);
            break;
        }
    }

    if (out.cap > 0) out.buf[out.len < out.cap ? out.len : out.cap - 1] = '\0';
    return out.len;
}

usize ksnprintf(char* buf, usize size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const usize n = kvsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

void kprintf(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    kvsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    serial_write(line);
    fbconsole::write(line);
}

void klog(LogLevel level, const char* fmt, ...) {
    if (!klog_enabled(level)) return;

    static const char* kTags[] = { "[err ] ", "[warn] ", "[info] ", "[dbg ] " };

    // One buffer, one write: the serial lock is per write, so a line built
    // from several writes could be split by another CPU's output.
    char line[512];
    const usize tag = ksnprintf(line, sizeof(line), "%s", kTags[static_cast<u8>(level)]);
    va_list args;
    va_start(args, fmt);
    usize len = tag + kvsnprintf(line + tag, sizeof(line) - tag, fmt, args);
    va_end(args);

    if (len > sizeof(line) - 2) len = sizeof(line) - 2;  // keep room for the newline
    line[len] = '\n';
    line[len + 1] = '\0';
    serial_write(line);
    fbconsole::write(line);
}
