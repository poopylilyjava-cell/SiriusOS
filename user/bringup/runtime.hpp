#pragma once
// The whole "C library" of the bring-up programs: system call stubs and just
// enough formatting to print a line. Real programs use mlibc (user/libc);
// nothing outside user/bringup/ may depend on this.
//
// Programs define `int main(int argc, char** argv, char** envp)`; crt0.S
// calls it and exits with its result.
#include <sirius/syscall.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int main(int argc, char** argv, char** envp);

namespace rt {

inline long syscall6(long number, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
    long result;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return result;
}

template <typename T> long arg(T value) { return (long)value; }

inline long write(int fd, const void* buffer, size_t length) { return syscall6(SYS_WRITE, fd, arg(buffer), (long)length); }
inline long read(int fd, void* buffer, size_t length)        { return syscall6(SYS_READ, fd, arg(buffer), (long)length); }
inline long open(const char* path, int flags)                 { return syscall6(SYS_OPEN, arg(path), flags, 0); }
inline long close(int fd)                                     { return syscall6(SYS_CLOSE, fd); }
inline long stat(const char* path, sirius_stat* out)          { return syscall6(SYS_STAT, arg(path), arg(out)); }
inline long fstat(int fd, sirius_stat* out)                   { return syscall6(SYS_FSTAT, fd, arg(out)); }
inline long lseek(int fd, long offset, int whence)            { return syscall6(SYS_LSEEK, fd, offset, whence); }
inline long readdir(int fd, sirius_dirent* out)               { return syscall6(SYS_READDIR, fd, arg(out)); }
inline long mkdir(const char* path)                           { return syscall6(SYS_MKDIR, arg(path), 0755); }
inline long unlink(const char* path)                          { return syscall6(SYS_UNLINK, arg(path)); }
inline long chdir(const char* path)                           { return syscall6(SYS_CHDIR, arg(path)); }
inline long getcwd(char* buffer, size_t size)                 { return syscall6(SYS_GETCWD, arg(buffer), (long)size); }
inline long fork()                                            { return syscall6(SYS_FORK); }
inline long execve(const char* path, const char* const* argv, const char* const* envp) {
    return syscall6(SYS_EXECVE, arg(path), arg(argv), arg(envp));
}
inline long waitpid(long pid, int* status, int options)       { return syscall6(SYS_WAITPID, pid, arg(status), options); }
inline void* mmap(void* address, size_t length, int protection, int flags, int fd, long offset) {
    return (void*)syscall6(SYS_MMAP, arg(address), (long)length, protection, flags, fd, offset);
}
inline long munmap(void* address, size_t length)              { return syscall6(SYS_MUNMAP, arg(address), (long)length); }
[[noreturn]] inline void exit(long status) {
    syscall6(SYS_EXIT, status);
    __builtin_unreachable();
}
inline void yield()                { syscall6(SYS_YIELD); }
inline void sleep_ms(long ms)      { syscall6(SYS_SLEEP_MS, ms); }
inline long getcpu()               { return syscall6(SYS_GETCPU); }
inline long getpid()               { return syscall6(SYS_GETPID); }
inline long getppid()              { return syscall6(SYS_GETPPID); }

inline bool is_error(long result) { return result < 0 && result > -4096; }

inline size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

inline bool streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

inline bool memeq(const void* a, const void* b, size_t n) {
    const auto* x = static_cast<const unsigned char*>(a);
    const auto* y = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return false;
    return true;
}

// Builds one line in a fixed buffer, so it goes out in a single write() and
// cannot be split by another process's output.
class Line {
public:
    Line& str(const char* s) {
        while (*s && m_length + 1 < sizeof(m_buffer)) m_buffer[m_length++] = *s++;
        return *this;
    }
    Line& num(long value) {
        char digits[24];
        int count = 0;
        unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value)
                                            : static_cast<unsigned long>(value);
        do { digits[count++] = static_cast<char>('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
        if (value < 0) str("-");
        while (count) { char c[2] = { digits[--count], 0 }; str(c); }
        return *this;
    }
    void print() {
        m_buffer[m_length++] = '\n';
        write(1, m_buffer, m_length);
        m_length = 0;
    }

private:
    char m_buffer[200];
    size_t m_length = 0;
};

} // namespace rt
