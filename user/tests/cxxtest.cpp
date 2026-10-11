// cxxtest: the M4 exit test, and since then the test of everything a hosted
// program can reach. A C++ program -- compiled by
// x86_64-sirius-g++, linked against mlibc and libstdc++ -- that leans on the
// parts of the runtime a freestanding program never touches: iostreams,
// containers and strings on the heap, exceptions, thread-local storage,
// static constructors and destructors, stdio and POSIX on top of the Sirius
// system calls.
//
// Prints one line per group of checks and exits 0 only if all of them pass.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sirius/fb.h>
#include <sirius/input.h>
#include <sirius/syscall.h>
#include <sirius/sysinfo.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    g_checks++;
    if (condition) return;
    g_failures++;
    std::cout << "[cxxtest] FAILED: " << what << std::endl;
}

// Constructed before main() by the .init_array the C library walks, and
// destroyed after it through __cxa_atexit.
struct Announcer {
    std::string name;
    explicit Announcer(std::string n) : name(std::move(n)) { constructed++; }
    ~Announcer() { std::printf("[cxxtest] static destructor ran for \"%s\"\n", name.c_str()); }
    static inline int constructed = 0;
};
Announcer g_announcer("a global object");

// Initialised from the executable's PT_TLS template; found through FS.
thread_local int t_counter = 42;
thread_local std::string t_text = "thread-local";

void test_strings_and_containers() {
    std::vector<std::string> words{"sirius", "is", "the", "brightest", "star"};
    std::sort(words.begin(), words.end());
    check(words.front() == "brightest" && words.back() == "the", "std::sort of std::vector<std::string>");

    std::string joined;
    for (const auto& word : words) joined += word + ' ';
    check(joined == "brightest is sirius star the ", "std::string concatenation");
    check(joined.find("sirius") == 13 && joined.substr(13, 6) == "sirius", "std::string find and substr");

    // Enough elements that the vector outgrows the allocator's small pools
    // and its storage comes from mmap.
    std::vector<int> numbers(1'000'000);
    std::iota(numbers.begin(), numbers.end(), 1);
    check(std::accumulate(numbers.begin(), numbers.end(), 0LL) == 500000500000LL, "a million-element std::vector");

    std::map<std::string, int> lengths;
    for (const auto& word : words) lengths[word] = static_cast<int>(word.size());
    check(lengths.size() == 5 && lengths["brightest"] == 9, "std::map");

    auto owned = std::make_unique<std::vector<double>>(1000, 0.5);
    check(std::accumulate(owned->begin(), owned->end(), 0.0) == 500.0, "std::unique_ptr and floating point");

    std::cout << "[cxxtest] strings and containers: " << joined << std::endl;
}

void test_streams() {
    std::ostringstream out;
    out << "pi=" << std::fixed << std::setprecision(4) << std::acos(-1.0)
        << " hex=" << std::hex << 48879 << " dec=" << std::dec << -17
        << " bool=" << std::boolalpha << true;
    check(out.str() == "pi=3.1416 hex=beef dec=-17 bool=true", "std::ostringstream formatting: " + out.str());

    std::istringstream in("12 3.5 word");
    int integer = 0; double real = 0; std::string word;
    in >> integer >> real >> word;
    check(integer == 12 && real == 3.5 && word == "word" && in.eof(), "std::istringstream extraction");

    check(std::to_string(1234567890123LL) == "1234567890123" && std::stod("2.25") == 2.25, "std::to_string and std::stod");

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%5.2f|%-6s|%04d|%zu", 2.0 / 3.0, "ab", 42, sizeof(void*));
    check(std::strcmp(buffer, " 0.67|ab    |0042|8") == 0, std::string("snprintf: ") + buffer);

    std::cout << "[cxxtest] streams: " << out.str() << std::endl;
}

void test_exceptions() {
    bool caught = false;
    try {
        throw std::runtime_error("thrown on Sirius");
    } catch (const std::exception& e) {
        caught = std::string(e.what()) == "thrown on Sirius";
    }
    check(caught, "throw and catch std::runtime_error");

    // Thrown from inside libstdc++, and unwound through its frames.
    bool range = false;
    try {
        std::vector<int> three(3);
        three.at(7) = 1;
    } catch (const std::out_of_range&) {
        range = true;
    }
    check(range, "std::vector::at throws std::out_of_range");

    // Destructors run while unwinding.
    int destroyed = 0;
    struct Guard { int& count; ~Guard() { count++; } };
    try {
        Guard a{destroyed}, b{destroyed};
        throw 5;
    } catch (int value) {
        check(value == 5 && destroyed == 2, "unwinding runs destructors");
    }

    std::cout << "[cxxtest] exceptions: thrown, unwound and caught" << std::endl;
}

void test_runtime() {
    check(Announcer::constructed == 1 && g_announcer.name == "a global object", "static constructors ran before main");

    check(t_counter == 42 && t_text == "thread-local", "thread-local variables start from their initialisers");
    t_counter++;
    t_text += " storage";
    check(t_counter == 43 && t_text == "thread-local storage", "thread-local variables are writable");

    errno = 0;
    check(open("/no/such/file", O_RDONLY) == -1 && errno == ENOENT, "errno (itself thread-local) reports ENOENT");

    void* block = std::malloc(100);
    std::memset(block, 0xab, 100);
    block = std::realloc(block, 1 << 20);
    check(block != nullptr && static_cast<unsigned char*>(block)[99] == 0xab, "malloc and realloc");
    std::free(block);

    std::cout << "[cxxtest] runtime: constructors, thread-local storage, errno, malloc" << std::endl;
}

void test_files() {
    std::ifstream motd("/etc/motd");
    std::string line;
    check(motd.is_open() && std::getline(motd, line) && line.rfind("Welcome to Sirius OS", 0) == 0,
          "std::ifstream reads /etc/motd");

    const char* path = "/tmp/cxxtest.txt";
    {
        std::ofstream out(path);
        out << "line one\n" << 2 << ' ' << 3.5 << '\n';
        check(out.good(), "std::ofstream writes a file on /tmp");
    }
    {
        std::ifstream in(path);
        std::string first; int two = 0; double real = 0;
        std::getline(in, first);
        in >> two >> real;
        check(first == "line one" && two == 2 && real == 3.5, "std::ifstream reads it back");
    }

    struct stat st {};
    check(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 15, "stat reports its size");

    // The same through C stdio, appending and seeking.
    std::FILE* file = std::fopen(path, "a+");
    check(file != nullptr, "fopen for append");
    if (file) {
        std::fprintf(file, "appended %d\n", 7);
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::rewind(file);
        char first[32] = {};
        check(size == 26 && std::fgets(first, sizeof(first), file) && std::strcmp(first, "line one\n") == 0,
              "fprintf, fseek, ftell and fgets");
        std::fclose(file);
    }
    check(std::remove(path) == 0 && access(path, F_OK) == -1 && errno == ENOENT, "remove deletes it");

    // A directory, by both routes.
    check(mkdir("/tmp/cxxdir", 0755) == 0 && stat("/tmp/cxxdir", &st) == 0 && S_ISDIR(st.st_mode), "mkdir");
    check(unlink("/tmp/cxxdir") == -1 && errno == EISDIR, "unlink refuses a directory");
    check(rmdir("/tmp/cxxdir") == 0, "rmdir");

    std::vector<std::string> names;
    if (DIR* dir = opendir("/bin")) {
        while (const dirent* entry = readdir(dir)) names.emplace_back(entry->d_name);
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    check(std::binary_search(names.begin(), names.end(), "cxxtest") &&
          std::binary_search(names.begin(), names.end(), "init"), "opendir and readdir list /bin");

    char cwd[256];
    check(chdir("/etc") == 0 && getcwd(cwd, sizeof(cwd)) && std::strcmp(cwd, "/etc") == 0 &&
          access("motd", R_OK) == 0 && chdir("/") == 0, "chdir, getcwd and a relative path");

    const int fd = open("/etc/motd", O_RDONLY);
    check(isatty(STDOUT_FILENO) == 1 && isatty(fd) == 0 && errno == ENOTTY, "isatty tells the console from a file");

    // A duplicate shares the original's offset; dup2 onto an open descriptor closes it first.
    char byte[2] = {};
    const int copy = dup(fd);
    check(copy > fd && read(fd, &byte[0], 1) == 1 && read(copy, &byte[1], 1) == 1 && byte[0] == 'W' && byte[1] == 'e',
          "dup shares the file offset");
    check(dup2(fd, copy) == copy && close(fd) == 0 && read(copy, &byte[0], 1) == 1 && byte[0] == 'l' && close(copy) == 0,
          "dup2 keeps the file open through the new descriptor");

    std::cout << "[cxxtest] files: /etc/motd says \"" << line.substr(0, 20) << "...\"; /bin holds "
              << names.size() << " entries" << std::endl;
}

void test_time() {
    using clock = std::chrono::steady_clock;
    const auto before = clock::now();
    timespec request{0, 30'000'000};
    check(nanosleep(&request, nullptr) == 0, "nanosleep");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - before).count();
    check(elapsed >= 30 && elapsed < 1000, "std::chrono::steady_clock advances across a sleep (" +
                                           std::to_string(elapsed) + " ms)");
    std::cout << "[cxxtest] time: a 30 ms sleep took " << elapsed << " ms" << std::endl;
}

// The devices a desktop is built on: the framebuffer and the input queue.
void test_devices() {
    int fd = open("/etc/motd", O_RDONLY);
    sirius_fb_info info{};
    check(ioctl(fd, SIRIUS_FB_GET_INFO, &info) == -1 && errno == ENOTTY, "ioctl on an ordinary file gives ENOTTY");
    check(mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ENODEV,
          "a file has no memory of its own to share");
    close(fd);

    fd = open("/dev/fb0", O_RDWR);
    check(fd >= 0, "open /dev/fb0");
    if (fd >= 0) {
        check(ioctl(fd, SIRIUS_FB_GET_INFO, &info) == 0 && info.width >= 640 && info.height >= 480 && info.bpp == 32 &&
              info.pitch >= info.width * 4 && info.size == static_cast<uint64_t>(info.pitch) * info.height,
              "the framebuffer describes itself");
        check(ioctl(fd, 0x12345, &info) == -1 && errno == ENOTTY, "and rejects requests it does not know");
        check(mmap(nullptr, info.size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) == MAP_FAILED,
              "a device cannot be mapped privately");
        check(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>((info.size + 8191) & ~4095ull)) == MAP_FAILED,
              "nor past its end");

        auto* pixels = static_cast<uint32_t*>(mmap(nullptr, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        check(pixels != MAP_FAILED, "mmap of the whole framebuffer");
        if (pixels != MAP_FAILED) {
            // The last pixel of the last row: the mapping reaches the end.
            uint32_t& corner = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(pixels) +
                                                               static_cast<size_t>(info.pitch) * (info.height - 1) + (info.width - 1) * 4);
            const uint32_t saved = corner;
            corner = 0x00123456;
            check((corner & 0x00ffffff) == 0x00123456, "pixels written through the mapping read back");

            // A device mapping is the device in both processes after fork --
            // not a copy-on-write page that would hide the child's drawing.
            std::fflush(stdout);
            const pid_t pid = fork();
            if (pid == 0) { corner = 0x00abcdef; _exit(0); }
            int status = 0;
            waitpid(pid, &status, 0);
            check((corner & 0x00ffffff) == 0x00abcdef, "a forked child draws on the same framebuffer");
            corner = saved;
            check(munmap(pixels, info.size) == 0, "munmap of the framebuffer");
        }
        close(fd);
    }

    // Nobody is typing during the tests, so the queue is normally empty;
    // whatever is there comes in whole records.
    fd = open("/dev/input", O_RDONLY | O_NONBLOCK);
    check(fd >= 0, "open /dev/input");
    if (fd >= 0) {
        sirius_input_event events[8];
        const ssize_t n = read(fd, events, sizeof(events));
        check((n == -1 && errno == EAGAIN) || (n > 0 && n % static_cast<ssize_t>(sizeof(events[0])) == 0),
              "a non-blocking read of the input queue returns records, or EAGAIN");
        char small[4];
        check(read(fd, small, sizeof(small)) == -1 && errno == EINVAL, "a buffer smaller than one record is refused");
        close(fd);
    }

    // The clock counts nanoseconds, not scheduler ticks: consecutive readings
    // move, and by far less than a tick.
    timespec a{}, b{};
    clock_gettime(CLOCK_MONOTONIC, &a);
    do clock_gettime(CLOCK_MONOTONIC, &b); while (b.tv_sec == a.tv_sec && b.tv_nsec == a.tv_nsec);
    const long step = (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec);
    check(step > 0 && step < 5000000, "the monotonic clock resolves much less than a scheduler tick");

    // What the system monitor reads: the machine's state and its processes.
    static sirius_sysinfo system;
    static sirius_procinfo processes[64];
    auto system_call = [](long number, long a, long b) {
        long result;
        asm volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b) : "rcx", "r11", "memory");
        return result;
    };
    check(system_call(SYS_SYSINFO, reinterpret_cast<long>(&system), 0) == 0 && system.cpu_count >= 1 && system.tick_hz > 0 &&
          system.memory_free > 0 && system.memory_free < system.memory_total && system.uptime_ns > 0 &&
          system.cpu_ticks[0] > 0 && system.cpu_idle_ticks[0] <= system.cpu_ticks[0], "sysinfo describes the machine");
    const long count = system_call(SYS_PROCLIST, reinterpret_cast<long>(processes), 64);
    bool found_self = false, found_parent = false;
    for (long i = 0; i < count; i++) {
        if (processes[i].pid == static_cast<uint32_t>(getpid()))
            found_self = std::strcmp(processes[i].name, "cxxtest") == 0 && processes[i].parent == static_cast<uint32_t>(getppid()) &&
                         processes[i].state == SIRIUS_PROC_RUNNING;
        if (processes[i].pid == static_cast<uint32_t>(getppid())) found_parent = processes[i].state == SIRIUS_PROC_SLEEPING;
    }
    check(count >= 2 && found_self && found_parent, "proclist names this process as running and its waiting parent as sleeping");

    // The time of day comes from the battery-backed clock: some time after
    // this was written, and not the monotonic clock under another name.
    timespec wall{};
    clock_gettime(CLOCK_REALTIME, &wall);
    check(wall.tv_sec > 1760000000 && wall.tv_sec != b.tv_sec, "the real-time clock knows what year it is");

    std::cout << "[cxxtest] devices: framebuffer " << info.width << "x" << info.height << ", input queue, a "
              << step << " ns clock step" << std::endl;
}

void test_processes(char** envp) {
    std::cout << std::flush;
    std::fflush(stdout);

    // The child has its own copy of everything, the thread pointer included.
    t_counter = 100;
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        t_counter++;
        _exit(t_counter == 101 && t_text == "thread-local storage" && getppid() != 0 ? 33 : 1);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 33,
          "a forked child keeps its thread-local storage and exits with its own status");
    check(t_counter == 100, "the child's writes stay in the child");

    // /bin/hello is a bring-up program: no libc at all. It exits with 7.
    pid = fork();
    if (pid == 0) {
        char* const argv[] = {const_cast<char*>("hello"), const_cast<char*>("from"), const_cast<char*>("cxxtest"), nullptr};
        execve("/bin/hello", argv, envp);
        _exit(127);
    }
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7, "fork, execve and waitpid");
    check(waitpid(-1, &status, 0) == -1 && errno == ECHILD, "waitpid with no children gives ECHILD");

    std::cout << "[cxxtest] processes: pid " << getpid() << ", child of " << getppid() << std::endl;
}

} // namespace

int main(int argc, char** argv, char** envp) {
    std::cout << "[cxxtest] hello from C++ on mlibc: argv";
    for (int i = 0; i < argc; i++) std::cout << ' ' << argv[i];
    std::cout << std::endl;

    const char* milestone = std::getenv("SIRIUS");
    check(argc == 3 && std::string(argv[1]) == "one" && std::string(argv[2]) == "two", "argv arrives intact");
    check(milestone != nullptr && std::string(milestone) == "M4", "getenv reads the environment execve passed");

    test_strings_and_containers();
    test_streams();
    test_exceptions();
    test_runtime();
    test_files();
    test_time();
    test_devices();
    test_processes(envp);

    if (g_failures == 0) {
        std::cout << "[cxxtest] all " << g_checks << " checks passed" << std::endl;
        std::cout << "[cxxtest] M4 exit test passed" << std::endl;
        return 0;
    }
    std::cout << "[cxxtest] M4 exit test FAILED: " << g_failures << " of " << g_checks << " checks" << std::endl;
    return 1;
}
