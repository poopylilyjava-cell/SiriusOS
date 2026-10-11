// fstest: the M3 system calls, exercised from user space.
//
// Files and directories on both the disk (FAT32) and /tmp (tmpfs), devices,
// mmap, fork's copy-on-write, waitpid's corner cases, and the error codes a
// libc will depend on. Exits 0 only if every check passes.
#include "runtime.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what, long detail = 0) {
    g_checks++;
    if (condition) return;
    g_failures++;
    rt::Line().str("[fstest] FAILED: ").str(what).str(" (").num(detail).str(")").print();
}

int open_or_fail(const char* path, int flags) {
    const long fd = rt::open(path, flags);
    check(!rt::is_error(fd), path, fd);
    return static_cast<int>(fd);
}

bool write_file(const char* path, const void* data, size_t length) {
    const long fd = rt::open(path, SIRIUS_O_WRONLY | SIRIUS_O_CREAT | SIRIUS_O_TRUNC);
    if (rt::is_error(fd)) return false;
    const long n = rt::write(static_cast<int>(fd), data, length);
    rt::close(static_cast<int>(fd));
    return n == static_cast<long>(length);
}

// The same checks on whichever filesystem `base` lives on. `orphans`: does it
// let an open file be unlinked, keeping the data until the last close?
void test_files(const char* label, const char* dir, const char* file, const char* nested, bool orphans) {
    rt::Line().str("[fstest] files on ").str(label).print();

    check(rt::mkdir(dir) == 0, "mkdir");
    check(rt::mkdir(dir) == -SIRIUS_EEXIST, "mkdir again gives EEXIST");

    // 40 KiB of a pattern: many FAT clusters, several tmpfs pages.
    static char data[40 * 1024];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = static_cast<char>(i * 7 + 3);
    check(write_file(file, data, sizeof(data)), "write 40 KiB");

    sirius_stat st;
    check(rt::stat(file, &st) == 0 && st.st_size == sizeof(data), "stat reports the size", static_cast<long>(st.st_size));
    check((st.st_mode & SIRIUS_S_IFMT) == SIRIUS_S_IFREG, "stat reports a regular file");

    int fd = open_or_fail(file, SIRIUS_O_RDWR);
    static char back[40 * 1024];
    check(rt::read(fd, back, sizeof(back)) == sizeof(data) && rt::memeq(back, data, sizeof(data)), "read it back");
    check(rt::read(fd, back, 10) == 0, "read at end of file returns 0");

    check(rt::lseek(fd, 1000, SIRIUS_SEEK_SET) == 1000, "lseek SET");
    check(rt::write(fd, "PATCH", 5) == 5, "overwrite in the middle");
    check(rt::lseek(fd, -5, SIRIUS_SEEK_CUR) == 1000, "lseek CUR backwards");
    char five[5];
    check(rt::read(fd, five, 5) == 5 && rt::memeq(five, "PATCH", 5), "the overwrite reads back");
    check(rt::lseek(fd, 0, SIRIUS_SEEK_END) == sizeof(data), "lseek END");
    check(rt::lseek(fd, -1, SIRIUS_SEEK_SET) == -SIRIUS_EINVAL, "a negative offset is refused");
    check(rt::fstat(fd, &st) == 0 && st.st_size == sizeof(data), "fstat");
    rt::close(fd);

    // Append, then truncate on open.
    fd = open_or_fail(file, SIRIUS_O_WRONLY | SIRIUS_O_APPEND);
    rt::write(fd, "!", 1);
    rt::close(fd);
    check(rt::stat(file, &st) == 0 && st.st_size == sizeof(data) + 1, "O_APPEND adds at the end");
    fd = open_or_fail(file, SIRIUS_O_WRONLY | SIRIUS_O_TRUNC);
    rt::close(fd);
    check(rt::stat(file, &st) == 0 && st.st_size == 0, "O_TRUNC empties the file");

    // Directory listing.
    check(write_file(nested, "n", 1), "a second file");
    fd = open_or_fail(dir, SIRIUS_O_RDONLY | SIRIUS_O_DIRECTORY);
    sirius_dirent entry;
    int entries = 0;
    while (rt::readdir(fd, &entry) == 1) entries++;
    rt::close(fd);
    check(entries == 2, "readdir lists both files", entries);

    // Removal and its rules.
    check(rt::unlink(dir) == -SIRIUS_ENOTEMPTY, "a non-empty directory stays");
    fd = open_or_fail(nested, SIRIUS_O_RDONLY);
    if (orphans) {
        // The name goes at once; the file lives on behind the descriptor.
        char byte = 0;
        check(rt::unlink(nested) == 0, "an open file can be unlinked");
        check(rt::stat(nested, &st) == -SIRIUS_ENOENT, "its name is gone");
        check(rt::read(fd, &byte, 1) == 1, "and its contents are still readable");
        rt::close(fd);
        check(rt::unlink(file) == 0, "unlink the other file");
    } else {
        check(rt::unlink(nested) == -SIRIUS_EBUSY, "an open file cannot be unlinked");
        rt::close(fd);
        check(rt::unlink(nested) == 0 && rt::unlink(file) == 0, "unlink both files");
    }
    check(rt::unlink(dir) == 0, "then the directory");
    check(rt::stat(dir, &st) == -SIRIUS_ENOENT, "it is gone");
}

void test_errors() {
    rt::Line().str("[fstest] error codes").print();
    sirius_stat st;
    check(rt::open("/no/such/file", SIRIUS_O_RDONLY) == -SIRIUS_ENOENT, "ENOENT");
    check(rt::open("/etc/motd/x", SIRIUS_O_RDONLY) == -SIRIUS_ENOTDIR, "ENOTDIR through a file");
    check(rt::open("/etc", SIRIUS_O_WRONLY) == -SIRIUS_EISDIR, "EISDIR for writing a directory");
    check(rt::read(99, &st, 1) == -SIRIUS_EBADF, "EBADF for a closed descriptor");
    check(rt::stat(reinterpret_cast<const char*>(0xffffffff80000000UL), &st) == -SIRIUS_EFAULT, "EFAULT for a kernel path pointer");
    const long fd = rt::open("/etc/motd", SIRIUS_O_RDONLY);
    check(rt::read(static_cast<int>(fd), reinterpret_cast<void*>(0xffffffff80001000UL), 8) == -SIRIUS_EFAULT,
          "EFAULT for a read into kernel memory");
    check(rt::write(static_cast<int>(fd), "x", 1) == -SIRIUS_EBADF, "EBADF for writing a read-only descriptor");
    rt::close(static_cast<int>(fd));

    // Running out of descriptors.
    static long fds[300];
    int opened = 0;
    for (; opened < 300; opened++) {
        fds[opened] = rt::open("/dev/null", SIRIUS_O_RDONLY);
        if (rt::is_error(fds[opened])) break;
    }
    check(opened < 300 && fds[opened] == -SIRIUS_EMFILE, "EMFILE when the table is full", opened);
    for (int i = 0; i < opened; i++) rt::close(static_cast<int>(fds[i]));
}

void test_devices() {
    rt::Line().str("[fstest] devices").print();
    int fd = open_or_fail("/dev/null", SIRIUS_O_RDWR);
    check(rt::write(fd, "discarded", 9) == 9, "/dev/null swallows writes");
    char c;
    check(rt::read(fd, &c, 1) == 0, "/dev/null reads as end of file");
    rt::close(fd);

    fd = open_or_fail("/dev/zero", SIRIUS_O_RDONLY);
    char zeros[64];
    for (auto& z : zeros) z = 1;
    check(rt::read(fd, zeros, sizeof(zeros)) == sizeof(zeros), "/dev/zero fills a buffer");
    bool all_zero = true;
    for (char z : zeros) if (z) all_zero = false;
    check(all_zero, "with zeros");
    check(rt::lseek(fd, 0, SIRIUS_SEEK_SET) == -SIRIUS_ESPIPE, "a character device cannot seek");
    rt::close(fd);

    fd = open_or_fail("/dev", SIRIUS_O_RDONLY);
    sirius_dirent entry;
    bool console = false;
    while (rt::readdir(fd, &entry) == 1) if (rt::streq(entry.d_name, "console")) console = entry.d_type == SIRIUS_DT_CHR;
    rt::close(fd);
    check(console, "/dev lists the console as a character device");
}

void test_cwd() {
    rt::Line().str("[fstest] working directory").print();
    char cwd[256];
    check(rt::getcwd(cwd, sizeof(cwd)) == 1 && rt::streq(cwd, "/"), "the cwd starts at /");
    check(rt::chdir("/etc") == 0, "chdir /etc");
    check(rt::getcwd(cwd, sizeof(cwd)) > 0 && rt::streq(cwd, "/etc"), "getcwd follows", rt::getcwd(cwd, sizeof(cwd)));
    sirius_stat st;
    check(rt::stat("motd", &st) == 0, "a relative path resolves from the cwd");
    check(rt::chdir("motd") == -SIRIUS_ENOTDIR, "chdir to a file fails");
    check(rt::chdir("../tmp") == 0 && rt::getcwd(cwd, sizeof(cwd)) > 0 && rt::streq(cwd, "/tmp"), "chdir with ..");
    check(rt::getcwd(cwd, 2) == -SIRIUS_EINVAL, "a short getcwd buffer is refused");
    rt::chdir("/");
}

void test_mmap() {
    rt::Line().str("[fstest] mmap").print();
    const size_t length = 5 * 4096;
    auto* anon = static_cast<unsigned char*>(rt::mmap(nullptr, length, SIRIUS_PROT_READ | SIRIUS_PROT_WRITE,
                                                       SIRIUS_MAP_PRIVATE | SIRIUS_MAP_ANONYMOUS, -1, 0));
    check(!rt::is_error(reinterpret_cast<long>(anon)), "anonymous mmap", reinterpret_cast<long>(anon));
    if (rt::is_error(reinterpret_cast<long>(anon))) return;
    check((reinterpret_cast<uintptr_t>(anon) & 4095) == 0, "mappings are page aligned");
    bool zero = true;
    for (size_t i = 0; i < length; i += 512) if (anon[i]) zero = false;
    check(zero, "anonymous memory starts zeroed");
    for (size_t i = 0; i < length; i++) anon[i] = static_cast<unsigned char>(i);
    check(anon[length - 1] == static_cast<unsigned char>(length - 1), "and is writable throughout");

    // Copy-on-write across fork: the child's writes stay in the child.
    const long pid = rt::fork();
    if (pid == 0) {
        for (size_t i = 0; i < length; i++) anon[i] = 0xee;
        rt::exit(anon[100] == 0xee ? 0 : 1);
    }
    int status = -1;
    check(rt::waitpid(pid, &status, 0) == pid && SIRIUS_WIFEXITED(status) && SIRIUS_WEXITSTATUS(status) == 0,
          "the child wrote its copy", status);
    check(anon[100] == 100 && anon[length - 1] == static_cast<unsigned char>(length - 1),
          "the parent's copy is untouched");
    check(rt::munmap(anon, length) == 0, "munmap");

    // A private file mapping matches the file.
    const long fd = rt::open("/etc/motd", SIRIUS_O_RDONLY);
    sirius_stat st;
    rt::fstat(static_cast<int>(fd), &st);
    char contents[512] = {};
    const long n = rt::read(static_cast<int>(fd), contents, sizeof(contents));
    auto* mapped = static_cast<const char*>(rt::mmap(nullptr, 4096, SIRIUS_PROT_READ, SIRIUS_MAP_PRIVATE, static_cast<int>(fd), 0));
    check(!rt::is_error(reinterpret_cast<long>(mapped)) && n > 0 && rt::memeq(mapped, contents, static_cast<size_t>(n)),
          "a file mapping holds the file's contents");
    check(!rt::is_error(reinterpret_cast<long>(mapped)) && mapped[st.st_size] == 0, "and zeros past its end");
    rt::close(static_cast<int>(fd));   // the mapping outlives the descriptor
    check(mapped[0] == contents[0], "the mapping outlives the descriptor");
    // A file on a disk cannot be mapped shared: there is no page cache to
    // share. (Shared memory proper is ipctest's subject.)
    const long again = rt::open("/etc/motd", SIRIUS_O_RDONLY);
    check(rt::mmap(nullptr, 4096, SIRIUS_PROT_READ, SIRIUS_MAP_SHARED, static_cast<int>(again), 0) ==
          reinterpret_cast<void*>(-SIRIUS_ENODEV), "a file on disk cannot be mapped shared");
    rt::close(static_cast<int>(again));
}

void test_processes() {
    rt::Line().str("[fstest] processes").print();
    int status;
    check(rt::waitpid(-1, &status, 0) == -SIRIUS_ECHILD, "waitpid without children");

    // WNOHANG on a child that is still running.
    long pid = rt::fork();
    if (pid == 0) { rt::sleep_ms(100); rt::exit(3); }
    check(rt::waitpid(pid, &status, SIRIUS_WNOHANG) == 0, "WNOHANG returns 0 while the child runs");
    check(rt::waitpid(pid, &status, 0) == pid && SIRIUS_WEXITSTATUS(status) == 3, "then it is reaped", status);

    // A child killed by a fault reports the signal.
    pid = rt::fork();
    if (pid == 0) {
        volatile int* volatile null_page = reinterpret_cast<int*>(8);   // opaque to the optimiser
        *null_page = 1;
        rt::exit(0);
    }
    check(rt::waitpid(pid, &status, 0) == pid && SIRIUS_WIFSIGNALED(status) && SIRIUS_WTERMSIG(status) == 11,
          "a faulting child dies of SIGSEGV", status);

    // Several children, reaped in whatever order they finish.
    for (int i = 0; i < 4; i++) {
        pid = rt::fork();
        if (pid == 0) { rt::sleep_ms(10 * (4 - i)); rt::exit(10 + i); }
    }
    int sum = 0;
    for (int i = 0; i < 4; i++) {
        if (rt::waitpid(-1, &status, 0) > 0) sum += SIRIUS_WEXITSTATUS(status);
    }
    check(sum == 10 + 11 + 12 + 13, "four children reaped by waitpid(-1)", sum);

    // Open files are shared with the child, offset included.
    const long fd = rt::open("/tmp/shared-offset", SIRIUS_O_RDWR | SIRIUS_O_CREAT | SIRIUS_O_TRUNC);
    pid = rt::fork();
    if (pid == 0) { rt::write(static_cast<int>(fd), "child", 5); rt::exit(0); }
    rt::waitpid(pid, &status, 0);
    rt::write(static_cast<int>(fd), "parent", 6);
    sirius_stat st;
    rt::fstat(static_cast<int>(fd), &st);
    check(st.st_size == 11, "a forked descriptor shares its offset", static_cast<long>(st.st_size));
    rt::close(static_cast<int>(fd));
    rt::unlink("/tmp/shared-offset");

    // execve failure returns to the caller intact.
    const char* argv[] = {"nope", nullptr};
    check(rt::execve("/bin/does-not-exist", argv, nullptr) == -SIRIUS_ENOENT, "execve of a missing file");
    check(rt::execve("/etc/motd", argv, nullptr) == -SIRIUS_ENOEXEC, "execve of a non-ELF file");
}

} // namespace

extern "C" int main(int, char**, char**) {
    rt::Line().str("[fstest] pid ").num(rt::getpid()).str(" parent ").num(rt::getppid()).print();

    test_files("the disk (fat32)", "/fstest", "/fstest/A rather long file name.bin", "/fstest/second", false);
    test_files("/tmp (tmpfs)", "/tmp/fstest", "/tmp/fstest/data.bin", "/tmp/fstest/second", true);
    test_errors();
    test_devices();
    test_cwd();
    test_mmap();
    test_processes();

    if (g_failures == 0) {
        rt::Line().str("[fstest] all ").num(g_checks).str(" checks passed").print();
        return 0;
    }
    rt::Line().str("[fstest] ").num(g_failures).str(" of ").num(g_checks).str(" checks FAILED").print();
    return 1;
}
