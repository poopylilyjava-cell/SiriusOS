// init: the first program, and the M3 exit test.
//
//   1. read a file from the disk (/etc/motd) and print it
//   2. write a file to the disk and read it back (CI then checks the disk
//      image with mtools, so another FAT implementation agrees)
//   3. fork; the child execs /bin/hello, the parent waits for its status --
//      and checks that the fork really copied, not shared, its memory
//   4. fork and exec /bin/fstest, the system call test suite
//   5. fork and exec /bin/cxxtest, the M4 exit test: a C++ program built by
//      the x86_64-sirius toolchain against mlibc and libstdc++. An image
//      made without that toolchain does not carry it, and the step is skipped.
#include "runtime.hpp"

namespace {

int g_failures = 0;

void fail(const char* what, long detail = 0) {
    rt::Line().str("[init] FAILED: ").str(what).str(" (").num(detail).str(")").print();
    g_failures++;
}

const char kWritten[] = "This file was written by Sirius OS from user space.\n";
const char kWrittenPath[] = "/Written by Sirius.txt";   // a long FAT name, on purpose

void read_motd() {
    const long fd = rt::open("/etc/motd", SIRIUS_O_RDONLY);
    if (rt::is_error(fd)) return fail("open /etc/motd", fd);
    char buffer[512];
    const long n = rt::read(static_cast<int>(fd), buffer, sizeof(buffer) - 1);
    rt::close(static_cast<int>(fd));
    if (n <= 0) return fail("read /etc/motd", n);
    buffer[n] = '\0';
    rt::Line().str("[init] /etc/motd says: ").str(buffer).print();
}

void write_and_read_back() {
    long fd = rt::open(kWrittenPath, SIRIUS_O_WRONLY | SIRIUS_O_CREAT | SIRIUS_O_TRUNC);
    if (rt::is_error(fd)) return fail("create the test file", fd);
    const long written = rt::write(static_cast<int>(fd), kWritten, sizeof(kWritten) - 1);
    rt::close(static_cast<int>(fd));
    if (written != static_cast<long>(sizeof(kWritten) - 1)) return fail("write the test file", written);

    char buffer[128] = {};
    fd = rt::open(kWrittenPath, SIRIUS_O_RDONLY);
    if (rt::is_error(fd)) return fail("reopen the test file", fd);
    const long n = rt::read(static_cast<int>(fd), buffer, sizeof(buffer));
    rt::close(static_cast<int>(fd));
    if (n != written || !rt::memeq(buffer, kWritten, static_cast<size_t>(n))) return fail("read back the test file", n);
    rt::Line().str("[init] wrote and read back \"").str(kWrittenPath).str("\"").print();
}

// Fork and exec `path`; return the child's wait status, or -1.
int run(const char* path, const char* const* argv, const char* const* envp, int& memory_check) {
    const long pid = rt::fork();
    if (rt::is_error(pid)) { fail("fork", pid); return -1; }

    if (pid == 0) {
        // The child's copy of memory_check is its own: the parent must not
        // see this store.
        memory_check = -1;
        const long error = rt::execve(path, argv, envp);
        rt::Line().str("[init child] execve ").str(path).str(" failed: ").num(error).print();
        rt::exit(127);
    }

    int status = 0;
    const long reaped = rt::waitpid(pid, &status, 0);
    if (reaped != pid) { fail("waitpid", reaped); return -1; }
    return status;
}

} // namespace

extern "C" int main(int, char**, char**) {
    rt::Line().str("[init] pid ").num(rt::getpid()).str(" on cpu ").num(rt::getcpu()).print();

    read_motd();
    write_and_read_back();

    int memory_check = 42;
    const char* hello_argv[] = {"hello", "from", "the", "child", nullptr};
    const char* hello_envp[] = {"SIRIUS=M3", nullptr};
    const int status = run("/bin/hello", hello_argv, hello_envp, memory_check);
    if (!SIRIUS_WIFEXITED(status) || SIRIUS_WEXITSTATUS(status) != 7) fail("hello's exit status", status);
    else rt::Line().str("[init] child exited with status ").num(SIRIUS_WEXITSTATUS(status)).print();
    if (memory_check != 42) fail("fork shared memory instead of copying it", memory_check);

    const char* fstest_argv[] = {"fstest", nullptr};
    const int fstest = run("/bin/fstest", fstest_argv, nullptr, memory_check);
    if (!SIRIUS_WIFEXITED(fstest) || SIRIUS_WEXITSTATUS(fstest) != 0) fail("fstest", fstest);

    sirius_stat st;
    if (rt::stat("/bin/cxxtest", &st) == 0) {
        const char* cxxtest_argv[] = {"cxxtest", "one", "two", nullptr};
        const char* cxxtest_envp[] = {"SIRIUS=M4", nullptr};
        const int cxxtest = run("/bin/cxxtest", cxxtest_argv, cxxtest_envp, memory_check);
        if (!SIRIUS_WIFEXITED(cxxtest) || SIRIUS_WEXITSTATUS(cxxtest) != 0) fail("cxxtest", cxxtest);
    } else {
        rt::Line().str("[init] /bin/cxxtest is not installed: skipping the M4 exit test").print();
    }

    // The M6 exit test: signals, pipes, sockets that pass descriptors,
    // shared memory, poll and epoll.
    if (rt::stat("/bin/ipctest", &st) == 0) {
        const char* ipctest_argv[] = {"ipctest", nullptr};
        const int ipctest = run("/bin/ipctest", ipctest_argv, nullptr, memory_check);
        if (!SIRIUS_WIFEXITED(ipctest) || SIRIUS_WEXITSTATUS(ipctest) != 0) fail("ipctest", ipctest);
    } else {
        rt::Line().str("[init] /bin/ipctest is not installed: skipping the M6 exit test").print();
    }

    // Threads: several in one process, and everything they share.
    if (rt::stat("/bin/threadtest", &st) == 0) {
        const char* threadtest_argv[] = {"threadtest", nullptr};
        const int threadtest = run("/bin/threadtest", threadtest_argv, nullptr, memory_check);
        if (!SIRIUS_WIFEXITED(threadtest) || SIRIUS_WEXITSTATUS(threadtest) != 0) fail("threadtest", threadtest);
    } else {
        rt::Line().str("[init] /bin/threadtest is not installed: skipping the threads test").print();
    }

    // Pseudo-terminals, sessions and process groups: what a terminal
    // emulator needs (M8).
    if (rt::stat("/bin/ptytest", &st) == 0) {
        const char* ptytest_argv[] = {"ptytest", nullptr};
        const int ptytest = run("/bin/ptytest", ptytest_argv, nullptr, memory_check);
        if (!SIRIUS_WIFEXITED(ptytest) || SIRIUS_WEXITSTATUS(ptytest) != 0) fail("ptytest", ptytest);
    } else {
        rt::Line().str("[init] /bin/ptytest is not installed: skipping the terminals test").print();
    }

    int ignored;
    const long none = rt::waitpid(-1, &ignored, 0);
    if (none != -SIRIUS_ECHILD) fail("waitpid with no children left", none);

    if (g_failures == 0) {
        rt::Line().str("[init] M3 exit test passed").print();
        return 0;
    }
    rt::Line().str("[init] M3 exit test FAILED: ").num(g_failures).str(" failure(s)").print();
    return 1;
}
