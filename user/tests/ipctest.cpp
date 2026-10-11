// ipctest: the M6 exit test. Everything a Wayland compositor and its clients
// need from the kernel to talk to each other: signals, pipes, unix sockets
// that pass file descriptors, shared memory, poll and epoll.
//
// The centrepiece is test_shared_buffer(): two processes exchange a
// shared-memory buffer by descriptor over a unix socket, both map it, and
// each sees what the other writes. That is a Wayland client attaching a
// buffer, minus the protocol.
//
// Prints one line per group of checks and exits 0 only if all of them pass.
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    g_checks++;
    if (condition) return;
    g_failures++;
    std::printf("[ipctest] FAILED: %s (errno %d)\n", what, errno);
}

long milliseconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Wait for `pid`; its exit status, or -signal if it was killed.
int reap(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1000;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

// --- signals ----------------------------------------------------------------------

volatile sig_atomic_t g_caught[65];
volatile sig_atomic_t g_last_signal;

void on_signal(int sig) {
    g_caught[sig] = g_caught[sig] + 1;
    g_last_signal = sig;
}

void on_signal_info(int sig, siginfo_t* info, void*) {
    if (info && info->si_signo == sig) g_caught[sig] = g_caught[sig] + 1;
}

void install(int sig, void (*handler)(int), int flags = 0) {
    struct sigaction action {};
    action.sa_handler = handler;
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void test_signals() {
    install(SIGUSR1, on_signal);
    check(raise(SIGUSR1) == 0 && g_caught[SIGUSR1] == 1, "a handler runs, and returns to where the program was");

    // Blocked: it waits. Unblocked: it arrives before sigprocmask returns.
    sigset_t usr1, old, pending;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigprocmask(SIG_BLOCK, &usr1, &old);
    raise(SIGUSR1);
    sigpending(&pending);
    check(g_caught[SIGUSR1] == 1 && sigismember(&pending, SIGUSR1), "a blocked signal stays pending");
    sigprocmask(SIG_SETMASK, &old, nullptr);
    sigpending(&pending);
    check(g_caught[SIGUSR1] == 2 && !sigismember(&pending, SIGUSR1), "and is delivered when unblocked");

    struct sigaction info_action {};
    info_action.sa_sigaction = on_signal_info;
    info_action.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &info_action, nullptr);
    raise(SIGUSR2);
    check(g_caught[SIGUSR2] == 1, "an SA_SIGINFO handler is told which signal");

    signal(SIGUSR2, SIG_IGN);
    check(raise(SIGUSR2) == 0 && g_caught[SIGUSR2] == 1, "an ignored signal does nothing");
    signal(SIGUSR2, SIG_DFL);

    // Default actions, seen from the parent.
    pid_t pid = fork();
    if (pid == 0) { raise(SIGTERM); _exit(0); }
    check(reap(pid) == -SIGTERM, "an unhandled SIGTERM kills, and the parent sees which signal");

    pid = fork();
    if (pid == 0) { pause(); _exit(0); }
    usleep(20000);
    check(kill(pid, SIGKILL) == 0 && reap(pid) == -SIGKILL, "kill() reaches a sleeping process");
    check(kill(pid, 0) == -1 && errno == ESRCH, "and afterwards there is no such process");

    pid = fork();
    if (pid == 0) { abort(); }
    check(reap(pid) == -SIGABRT, "abort() dies of SIGABRT");

    pid = fork();
    if (pid == 0) { *static_cast<volatile int*>(nullptr) = 1; _exit(0); }
    check(reap(pid) == -SIGSEGV, "a wild pointer dies of SIGSEGV");

    pid = fork();
    if (pid == 0) {
        install(SIGSEGV, [](int) { _exit(42); });
        *static_cast<volatile int*>(nullptr) = 1;
        _exit(0);
    }
    check(reap(pid) == 42, "unless the program handles it");

    // A child's exit raises SIGCHLD in the parent.
    install(SIGCHLD, on_signal);
    pid = fork();
    if (pid == 0) _exit(7);
    check(reap(pid) == 7 && g_caught[SIGCHLD] >= 1, "SIGCHLD announces a child's exit");
    signal(SIGCHLD, SIG_DFL);

    // A timer signal interrupts a sleep...
    install(SIGALRM, on_signal);
    itimerval once{ { 0, 0 }, { 0, 30000 } };
    const long start = milliseconds();
    setitimer(ITIMER_REAL, &once, nullptr);
    timespec second{ 1, 0 }, left{};
    check(nanosleep(&second, &left) == -1 && errno == EINTR && g_caught[SIGALRM] == 1,
          "SIGALRM interrupts a sleep with EINTR");
    const long woke_after = milliseconds() - start;
    check(woke_after >= 30 && woke_after < 500 && (left.tv_sec > 0 || left.tv_nsec > 400000000),
          "at the time asked for, with the rest of the sleep reported");

    // ...and arrives in the middle of a computation without disturbing it:
    // every register, flags and the FPU included, comes back as it was.
    itimerval repeating{ { 0, 10000 }, { 0, 10000 } };
    setitimer(ITIMER_REAL, &repeating, nullptr);
    const int before = g_caught[SIGALRM];
    uint64_t integer_sum = 0;
    double real_sum = 0.0;
    const long until = milliseconds() + 150;
    uint64_t rounds = 0;
    do {
        for (uint64_t i = 1; i <= 100000; i++) { integer_sum += i * i; real_sum += 1.0 / static_cast<double>(i); }
        rounds++;
    } while (milliseconds() < until);
    itimerval off{};
    setitimer(ITIMER_REAL, &off, nullptr);
    const uint64_t expected_integer = rounds * 333338333350000ULL;   // the sum of i*i for 1..100000
    double one_round = 0.0;
    for (uint64_t i = 1; i <= 100000; i++) one_round += 1.0 / static_cast<double>(i);
    double expected_real = 0.0;
    for (uint64_t r = 0; r < rounds; r++) for (uint64_t i = 1; i <= 100000; i++) expected_real += 1.0 / static_cast<double>(i);
    check(g_caught[SIGALRM] - before >= 5, "an interval timer keeps firing");
    check(integer_sum == expected_integer && real_sum == expected_real && one_round > 12.0,
          "handlers interrupting a computation leave its registers and FPU state intact");
    signal(SIGALRM, SIG_DFL);

    std::printf("[ipctest] signals: handlers, masks, default actions, SIGCHLD, timers (%d alarms during %llu rounds)\n",
                g_caught[SIGALRM] - before, static_cast<unsigned long long>(rounds));
}

// --- pipes ------------------------------------------------------------------------

void test_pipes() {
    int fds[2];
    check(pipe(fds) == 0, "pipe");
    char buffer[64] = {};
    check(write(fds[1], "through the pipe", 16) == 16 && read(fds[0], buffer, sizeof(buffer)) == 16 &&
          std::memcmp(buffer, "through the pipe", 16) == 0, "bytes written come out the other end");

    check(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0 && (fcntl(fds[0], F_GETFL) & O_NONBLOCK) &&
          read(fds[0], buffer, 1) == -1 && errno == EAGAIN, "an empty non-blocking pipe gives EAGAIN");
    fcntl(fds[0], F_SETFL, 0);

    // More than the pipe holds: writer and reader each have to wait for the
    // other, several times over.
    constexpr size_t kTotal = 300000;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        static char block[5000];
        size_t sent = 0;
        while (sent < kTotal) {
            const size_t n = kTotal - sent < sizeof(block) ? kTotal - sent : sizeof(block);
            for (size_t i = 0; i < n; i++) block[i] = static_cast<char>((sent + i) % 251);
            if (write(fds[1], block, n) != static_cast<ssize_t>(n)) _exit(1);
            sent += n;
        }
        _exit(0);
    }
    close(fds[1]);
    size_t received = 0;
    bool intact = true;
    for (;;) {
        static char block[7000];
        const ssize_t n = read(fds[0], block, sizeof(block));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++)
            if (block[i] != static_cast<char>((received + static_cast<size_t>(i)) % 251)) intact = false;
        received += static_cast<size_t>(n);
    }
    check(received == kTotal && intact && reap(pid) == 0, "300 kB cross a 64 kB pipe intact, and the writer's close ends the stream");
    close(fds[0]);

    // Nobody to read: EPIPE, and SIGPIPE unless ignored.
    pipe(fds);
    close(fds[0]);
    signal(SIGPIPE, SIG_IGN);
    check(write(fds[1], "x", 1) == -1 && errno == EPIPE, "writing to a pipe nobody reads gives EPIPE");
    signal(SIGPIPE, SIG_DFL);
    pid = fork();
    if (pid == 0) { write(fds[1], "x", 1); _exit(0); }
    check(reap(pid) == -SIGPIPE, "and kills with SIGPIPE by default");
    close(fds[1]);

    // The plumbing a shell does: a child's standard output redirected into
    // a pipe with dup2, across exec, into a program that knows nothing of it.
    pipe2(fds, O_CLOEXEC);
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        char* const argv[] = { const_cast<char*>("hello"), const_cast<char*>("down"), const_cast<char*>("the"), const_cast<char*>("pipe"), nullptr };
        char* const envp[] = { nullptr };
        execve("/bin/hello", argv, envp);
        _exit(127);
    }
    close(fds[1]);
    std::string output;
    for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;) output.append(buffer, static_cast<size_t>(n));
    close(fds[0]);
    check(reap(pid) == 7 && output.find("argv: hello down the pipe") != std::string::npos,
          "a child's stdout, redirected with dup2, arrives through the pipe after exec");

    std::printf("[ipctest] pipes: data, blocking both ways, EPIPE, dup2 plumbing (/bin/hello said %zu bytes)\n", output.size());
}

// --- poll -------------------------------------------------------------------------

void test_poll() {
    int fds[2];
    pipe(fds);
    pollfd watch[2] = { { fds[0], POLLIN, 0 }, { fds[1], POLLOUT, 0 } };

    check(poll(watch, 2, 0) == 1 && watch[0].revents == 0 && watch[1].revents == POLLOUT, "an empty pipe is writable and not readable");

    long start = milliseconds();
    check(poll(watch, 1, 60) == 0, "poll times out with nothing ready");
    const long waited = milliseconds() - start;
    check(waited >= 60 && waited < 400, "after the time asked for");

    write(fds[1], "x", 1);
    check(poll(watch, 1, -1) == 1 && watch[0].revents == POLLIN, "data makes the read end readable");

    // Woken from a blocking poll by another process writing.
    char byte;
    read(fds[0], &byte, 1);
    pid_t pid = fork();
    if (pid == 0) { usleep(40000); write(fds[1], "y", 1); _exit(0); }
    start = milliseconds();
    check(poll(watch, 1, 5000) == 1 && watch[0].revents == POLLIN && milliseconds() - start < 2000, "a sleeping poll wakes when data arrives");
    reap(pid);
    read(fds[0], &byte, 1);

    close(fds[1]);
    check(poll(watch, 1, 0) == 1 && (watch[0].revents & POLLHUP), "closing the write end hangs up the read end");
    close(fds[0]);
    pollfd closed = { fds[0], POLLIN, 0 }, skipped = { -1, POLLIN, 0x7f };
    check(poll(&closed, 1, 0) == 1 && closed.revents == POLLNVAL, "a closed descriptor is reported as invalid");
    check(poll(&skipped, 1, 0) == 0 && skipped.revents == 0, "a negative one is skipped");

    std::printf("[ipctest] poll: readiness, timeouts (%ld ms for 60), wakeups, hangup\n", waited);
}

// --- sockets ----------------------------------------------------------------------

// Send `length` bytes and `count` descriptors in one message.
ssize_t send_with_fds(int socket, const void* data, size_t length, const int* fds, int count) {
    char control[CMSG_SPACE(sizeof(int) * 32)];
    iovec piece = { const_cast<void*>(data), length };
    msghdr message{};
    message.msg_iov = &piece;
    message.msg_iovlen = 1;
    if (count) {
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * static_cast<size_t>(count));
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * static_cast<size_t>(count));
        std::memcpy(CMSG_DATA(header), fds, sizeof(int) * static_cast<size_t>(count));
    }
    return sendmsg(socket, &message, MSG_NOSIGNAL);
}

// Receive up to `capacity` bytes and whatever descriptors came with them.
ssize_t receive_with_fds(int socket, void* data, size_t capacity, int* fds, int* count, int flags = 0) {
    char control[CMSG_SPACE(sizeof(int) * 32)];
    iovec piece = { data, capacity };
    msghdr message{};
    message.msg_iov = &piece;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    *count = 0;
    const ssize_t n = recvmsg(socket, &message, flags);
    if (n < 0) return n;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
        *count = static_cast<int>((header->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        std::memcpy(fds, CMSG_DATA(header), sizeof(int) * static_cast<size_t>(*count));
    }
    return n;
}

void test_sockets() {
    int pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair");
    char buffer[128] = {};
    check(write(pair[0], "ping", 4) == 4 && read(pair[1], buffer, sizeof(buffer)) == 4 &&
          send(pair[1], "pong!", 5, 0) == 5 && recv(pair[0], buffer, sizeof(buffer), 0) == 5 && std::memcmp(buffer, "pong!", 5) == 0,
          "a socket pair carries bytes both ways");
    check(recv(pair[0], buffer, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN, "MSG_DONTWAIT on an empty socket gives EAGAIN");

    // A descriptor crosses the socket and arrives as a new one for the same
    // open file: here, the write end of a pipe.
    int pipe_fds[2], received[32], count = 0;
    pipe(pipe_fds);
    check(send_with_fds(pair[0], "fd", 2, &pipe_fds[1], 1) == 2, "sendmsg with SCM_RIGHTS");
    close(pipe_fds[1]);   // the copy in flight keeps the pipe's write end open
    pollfd watch = { pipe_fds[0], POLLIN, 0 };
    check(poll(&watch, 1, 0) == 0, "a descriptor in flight keeps its file open");
    check(receive_with_fds(pair[1], buffer, sizeof(buffer), received, &count, MSG_CMSG_CLOEXEC) == 2 && count == 1 &&
          received[0] != pipe_fds[1] + 1000 && (fcntl(received[0], F_GETFD) & FD_CLOEXEC),
          "recvmsg delivers it as a new descriptor, close-on-exec when asked");
    check(write(received[0], "via", 3) == 3 && read(pipe_fds[0], buffer, sizeof(buffer)) == 3 && std::memcmp(buffer, "via", 3) == 0,
          "and it is the same pipe");
    close(received[0]);
    check(read(pipe_fds[0], buffer, 1) == 0, "whose last writer it was");
    close(pipe_fds[0]);

    // Wayland sends up to 28 in one message.
    int many[28];
    for (int& fd : many) fd = open("/dev/null", O_RDONLY);
    check(send_with_fds(pair[0], "many", 4, many, 28) == 4 &&
          receive_with_fds(pair[1], buffer, sizeof(buffer), received, &count) == 4 && count == 28, "28 descriptors in one message");
    struct stat a {}, b {};
    fstat(many[27], &a);
    fstat(received[27], &b);
    check(a.st_ino == b.st_ino && S_ISCHR(b.st_mode), "arrive in order");
    for (int fd : many) close(fd);
    for (int i = 0; i < count; i++) close(received[i]);

    // Descriptors travel with the bytes they were sent with: two messages
    // are two receives, even if the data would fit in one.
    int first = open("/dev/null", O_RDONLY), second = open("/dev/zero", O_RDONLY);
    send_with_fds(pair[0], "one", 3, &first, 1);
    send_with_fds(pair[0], "two", 3, &second, 1);
    check(receive_with_fds(pair[1], buffer, sizeof(buffer), received, &count) == 3 && count == 1 && std::memcmp(buffer, "one", 3) == 0,
          "a receive stops at the next message that carries descriptors");
    close(received[0]);
    check(receive_with_fds(pair[1], buffer, sizeof(buffer), received, &count) == 3 && count == 1 && std::memcmp(buffer, "two", 3) == 0,
          "which the following receive returns");
    close(received[0]); close(first); close(second);

    close(pair[0]);
    watch = { pair[1], POLLIN | POLLOUT, 0 };
    check(poll(&watch, 1, 0) == 1 && (watch.revents & POLLHUP) && (watch.revents & POLLIN), "a closed peer hangs the socket up");
    check(read(pair[1], buffer, sizeof(buffer)) == 0, "reading then gives end of stream");
    check(send(pair[1], "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE, "and sending gives EPIPE");
    close(pair[1]);

    // A named socket: bind, listen, connect, accept.
    const char* path = "/tmp/ipctest.sock";
    unlink(path);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);

    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    check(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && errno == ENOENT,
          "connecting to a name that does not exist fails");
    close(client);

    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    check(listener >= 0 && bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind creates the socket's name");
    struct stat st {};
    check(stat(path, &st) == 0 && S_ISSOCK(st.st_mode), "which the filesystem shows as a socket");
    const int other = socket(AF_UNIX, SOCK_STREAM, 0);
    check(bind(other, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && errno == EADDRINUSE, "a name can be bound once");
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    check(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && errno == ECONNREFUSED,
          "connecting before listen() is refused");
    check(listen(listener, 8) == 0, "listen");
    check(accept(listener, nullptr, nullptr) == -1 && errno == EAGAIN, "a non-blocking accept with nobody waiting gives EAGAIN");

    const pid_t pid = fork();
    if (pid == 0) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) _exit(1);
        char reply[16] = {};
        if (write(fd, "hello server", 12) != 12 || read(fd, reply, sizeof(reply)) != 12 || std::memcmp(reply, "hello client", 12) != 0) _exit(2);
        _exit(0);
    }
    watch = { listener, POLLIN, 0 };
    check(poll(&watch, 1, 5000) == 1 && watch.revents == POLLIN, "a connection makes the listener readable");
    const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    check(connection >= 0, "accept");
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    check(getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 && credentials.pid == pid && credentials.uid == 0,
          "SO_PEERCRED names the client's process");
    check(read(connection, buffer, sizeof(buffer)) == 12 && std::memcmp(buffer, "hello server", 12) == 0 &&
          write(connection, "hello client", 12) == 12, "client and server talk");
    check(reap(pid) == 0, "and the client is satisfied");
    close(connection); close(client); close(other); close(listener);
    check(unlink(path) == 0, "the name is removed like a file");

    std::printf("[ipctest] sockets: pairs, SCM_RIGHTS (1 and 28 descriptors), named sockets, SO_PEERCRED\n");
}

// --- shared memory, by descriptor, over a socket -------------------------------------

uint32_t pattern(size_t index, uint32_t seed) { return static_cast<uint32_t>(index * 2654435761u) ^ seed; }

void test_shared_buffer() {
    constexpr size_t kBytes = 1024 * 1024, kWords = kBytes / sizeof(uint32_t);
    const char* path = "/tmp/ipctest-shm.sock";
    unlink(path);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);

    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(listener, 1);

    // The "client": makes a buffer, draws into it, hands it over.
    const pid_t pid = fork();
    if (pid == 0) {
        const int fd = memfd_create("buffer", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, kBytes) != 0) _exit(1);
        auto* words = static_cast<uint32_t*>(mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (words == MAP_FAILED) _exit(2);
        for (size_t i = 0; i < kWords; i++) words[i] = pattern(i, 0xc11e17);

        const int server = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) _exit(3);
        const uint32_t size = kBytes;
        if (send_with_fds(server, &size, sizeof(size), &fd, 1) != sizeof(size)) _exit(4);
        close(fd);   // the mapping, and the server's descriptor, keep the memory

        // The server answers once it has written its half.
        char done;
        if (read(server, &done, 1) != 1) _exit(5);
        for (size_t i = 0; i < kWords / 2; i++) if (words[i] != pattern(i, 0xc11e17)) _exit(6);
        for (size_t i = kWords / 2; i < kWords; i++) if (words[i] != pattern(i, 0x5e7fe7)) _exit(7);
        // And once more the other way, with both mappings long established.
        words[0] = 0xfeedface;
        if (write(server, "!", 1) != 1) _exit(8);
        _exit(0);
    }

    // The "compositor": receives the descriptor, maps it, reads the pixels.
    const int client = accept(listener, nullptr, nullptr);
    uint32_t size = 0;
    int fd = -1, count = 0;
    check(receive_with_fds(client, &size, sizeof(size), &fd, &count) == sizeof(size) && count == 1 && size == kBytes,
          "the buffer's descriptor arrives over the socket");
    struct stat st {};
    check(fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) == kBytes, "it is the size the sender made it");
    auto* words = static_cast<uint32_t*>(mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    check(words != MAP_FAILED, "and can be mapped by the receiver");
    bool intact = words != MAP_FAILED;
    for (size_t i = 0; intact && i < kWords; i++) intact = words[i] == pattern(i, 0xc11e17);
    check(intact, "where the receiver reads exactly what the sender wrote");

    if (words != MAP_FAILED) {
        for (size_t i = kWords / 2; i < kWords; i++) words[i] = pattern(i, 0x5e7fe7);
        write(client, "+", 1);
        char reply = 0;
        check(read(client, &reply, 1) == 1 && reply == '!' && words[0] == 0xfeedface,
              "and each side sees the other's later writes: one memory, mapped twice");

        // read() and write() on the descriptor reach the same memory.
        uint32_t word = 0;
        check(pread(fd, &word, sizeof(word), 0) == sizeof(word) && word == 0xfeedface, "the descriptor reads what the mapping holds");
        check(munmap(words, kBytes) == 0, "munmap");
    }
    check(reap(pid) == 0, "the sender saw the receiver's writes too");

    // Shrinking and growing: what is cut off is gone, what comes back is zero.
    check(ftruncate(fd, 4096) == 0 && ftruncate(fd, 8192) == 0 && fstat(fd, &st) == 0 && st.st_size == 8192, "ftruncate resizes it");
    auto* page = static_cast<uint32_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    check(page != MAP_FAILED && page[0] == 0xfeedface && page[1024] == 0, "keeping what remains and zero-filling what is new");
    check(mmap(nullptr, 16384, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED, "a mapping cannot reach past the end");

    // A shared mapping stays shared across fork; a private one does not.
    static int private_value = 1;
    const pid_t child = fork();
    if (child == 0) { page[1] = 0xabad1dea; private_value = 2; _exit(0); }
    check(reap(child) == 0 && page[1] == 0xabad1dea && private_value == 1, "fork shares a shared mapping and copies the rest");
    munmap(page, 8192);
    close(fd); close(client); close(listener);
    unlink(path);

    // Shared memory with no descriptor at all: for a process and the
    // children it forks.
    auto* counter = static_cast<volatile int*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    check(counter != MAP_FAILED && *counter == 0, "an anonymous shared mapping starts as zeros");
    if (counter != MAP_FAILED) {
        const pid_t writer = fork();
        if (writer == 0) { *counter = 1234; _exit(0); }
        check(reap(writer) == 0 && *counter == 1234, "and a forked child writes into the parent's view of it");
        munmap(const_cast<int*>(counter), 4096);
    }

    std::printf("[ipctest] shared memory: a 1 MiB buffer passed by descriptor and mapped coherently in two processes\n");
}

// --- epoll and the descriptors made for it ---------------------------------------------

void test_epoll() {
    const int set = epoll_create1(EPOLL_CLOEXEC);
    check(set >= 0, "epoll_create1");

    int fds[2];
    pipe(fds);
    const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    const int event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    sigset_t usr2;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    sigprocmask(SIG_BLOCK, &usr2, nullptr);
    const int signals = signalfd(-1, &usr2, SFD_CLOEXEC | SFD_NONBLOCK);
    check(timer >= 0 && event >= 0 && signals >= 0, "timerfd, eventfd and signalfd");

    epoll_event added{};
    const int members[4] = { fds[0], timer, event, signals };
    bool all = true;
    for (int i = 0; i < 4; i++) {
        added.events = EPOLLIN;
        added.data.u32 = static_cast<uint32_t>(i);
        all = all && epoll_ctl(set, EPOLL_CTL_ADD, members[i], &added) == 0;
    }
    check(all && epoll_ctl(set, EPOLL_CTL_ADD, timer, &added) == -1 && errno == EEXIST, "descriptors are added once each");

    epoll_event ready[8];
    long start = milliseconds();
    check(epoll_wait(set, ready, 8, 50) == 0 && milliseconds() - start >= 50, "nothing ready: epoll_wait times out");

    // A timer wakes the wait by itself: nothing else happens meanwhile.
    itimerspec in_30ms{ { 0, 0 }, { 0, 30000000 } };
    timerfd_settime(timer, 0, &in_30ms, nullptr);
    start = milliseconds();
    const int n = epoll_wait(set, ready, 8, 5000);
    const long fired_after = milliseconds() - start;
    uint64_t expirations = 0;
    check(n == 1 && ready[0].data.u32 == 1 && fired_after >= 30 && fired_after < 1000, "a timerfd wakes epoll_wait when it expires");
    check(read(timer, &expirations, sizeof(expirations)) == sizeof(expirations) && expirations == 1 &&
          read(timer, &expirations, sizeof(expirations)) == -1 && errno == EAGAIN, "and reads as one expiration, once");

    // Three things at once.
    write(fds[1], "x", 1);
    const uint64_t five = 5;
    write(event, &five, sizeof(five));
    raise(SIGUSR2);   // blocked: it stays pending, for the signalfd to collect
    int seen = 0;
    const int count = epoll_wait(set, ready, 8, 0);
    for (int i = 0; i < count; i++) seen |= 1 << ready[i].data.u32;
    check(count == 3 && seen == 0b1101, "a pipe, an eventfd and a signalfd are all reported ready together");

    uint64_t counter = 0;
    signalfd_siginfo info{};
    check(read(event, &counter, sizeof(counter)) == sizeof(counter) && counter == 5, "the eventfd reads back its count");
    check(read(signals, &info, sizeof(info)) == sizeof(info) && info.ssi_signo == SIGUSR2 && g_caught[SIGUSR2] == 1,
          "the signalfd hands over the signal, and no handler ran");
    sigset_t pending;
    sigpending(&pending);
    check(!sigismember(&pending, SIGUSR2), "which is then no longer pending");
    sigprocmask(SIG_UNBLOCK, &usr2, nullptr);

    // Level triggered: the pipe is still readable, so it is still reported.
    check(epoll_wait(set, ready, 8, 0) == 1 && ready[0].data.u32 == 0, "an unread pipe is reported again");
    added.events = EPOLLOUT;
    added.data.u32 = 9;
    check(epoll_ctl(set, EPOLL_CTL_MOD, fds[0], &added) == 0 && epoll_wait(set, ready, 8, 0) == 0, "EPOLL_CTL_MOD changes what is watched");
    check(epoll_ctl(set, EPOLL_CTL_DEL, fds[0], nullptr) == 0 && epoll_ctl(set, EPOLL_CTL_DEL, fds[0], nullptr) == -1 && errno == ENOENT,
          "EPOLL_CTL_DEL removes it");

    // Closing a watched descriptor takes it out of the set.
    write(event, &five, sizeof(five));
    check(epoll_wait(set, ready, 8, 0) == 1, "the eventfd is ready again");
    close(event);
    check(epoll_wait(set, ready, 8, 0) == 0, "and gone from the set once closed");

    close(fds[0]); close(fds[1]); close(timer); close(signals); close(set);
    std::printf("[ipctest] epoll: timerfd (fired after %ld ms for 30), eventfd, signalfd, modify and delete\n", fired_after);
}

// --- descriptors across exec ------------------------------------------------------------

// Run again as "ipctest check-fds <closed> <open>" to see the table the way a
// freshly executed program does.
int check_fds(int should_be_closed, int should_be_open) {
    const bool closed = fcntl(should_be_closed, F_GETFD) == -1 && errno == EBADF;
    const bool open = fcntl(should_be_open, F_GETFD) == 0;
    char byte = 0;
    return (closed ? 0 : 1) | (open && read(should_be_open, &byte, 1) == 1 && byte == 'W' ? 0 : 2);
}

void test_descriptors() {
    const int cloexec = open("/etc/motd", O_RDONLY | O_CLOEXEC);
    const int kept = open("/etc/motd", O_RDONLY);
    check((fcntl(cloexec, F_GETFD) & FD_CLOEXEC) && fcntl(kept, F_GETFD) == 0, "O_CLOEXEC marks the descriptor");

    const int duplicate = fcntl(cloexec, F_DUPFD, 100);
    check(duplicate >= 100 && fcntl(duplicate, F_GETFD) == 0, "F_DUPFD finds a descriptor at or above the one asked for, not close-on-exec");
    const int duplicate_cloexec = fcntl(kept, F_DUPFD_CLOEXEC, 0);
    check(duplicate_cloexec >= 0 && (fcntl(duplicate_cloexec, F_GETFD) & FD_CLOEXEC), "F_DUPFD_CLOEXEC makes one that is");
    check(fcntl(kept, F_SETFD, FD_CLOEXEC) == 0 && (fcntl(kept, F_GETFD) & FD_CLOEXEC) && fcntl(kept, F_SETFD, 0) == 0,
          "F_SETFD changes it");
    close(duplicate); close(duplicate_cloexec);

    const pid_t pid = fork();
    if (pid == 0) {
        char closed_arg[16], open_arg[16];
        std::snprintf(closed_arg, sizeof(closed_arg), "%d", cloexec);
        std::snprintf(open_arg, sizeof(open_arg), "%d", kept);
        char* const argv[] = { const_cast<char*>("ipctest"), const_cast<char*>("check-fds"), closed_arg, open_arg, nullptr };
        char* const envp[] = { nullptr };
        execve("/bin/ipctest", argv, envp);
        _exit(127);
    }
    check(reap(pid) == 0, "across exec, close-on-exec descriptors are closed and the others still work");
    close(cloexec); close(kept);

    // An open file outlives its name (on /tmp).
    const int fd = open("/tmp/ipctest-orphan", O_RDWR | O_CREAT | O_TRUNC, 0600);
    char buffer[8] = {};
    check(write(fd, "orphan", 6) == 6 && unlink("/tmp/ipctest-orphan") == 0 && access("/tmp/ipctest-orphan", F_OK) == -1,
          "a file can be unlinked while open");
    check(lseek(fd, 0, SEEK_SET) == 0 && read(fd, buffer, 6) == 6 && std::memcmp(buffer, "orphan", 6) == 0, "and still read through its descriptor");
    close(fd);

    // flock: held by an open file, so a second open of the same file is a
    // second party, and a duplicate of the first is not.
    const int lock_a = open("/tmp/ipctest-lock", O_RDWR | O_CREAT, 0600);
    const int lock_b = open("/tmp/ipctest-lock", O_RDWR);
    const int lock_a2 = dup(lock_a);
    check(flock(lock_a, LOCK_EX | LOCK_NB) == 0, "flock takes an exclusive lock");
    check(flock(lock_b, LOCK_EX | LOCK_NB) == -1 && errno == EAGAIN && flock(lock_b, LOCK_SH | LOCK_NB) == -1,
          "which another open of the file cannot get past");
    check(flock(lock_a2, LOCK_EX | LOCK_NB) == 0, "but a duplicate of the holder already has");
    check(flock(lock_a, LOCK_SH) == 0 && flock(lock_b, LOCK_SH | LOCK_NB) == 0, "shared locks coexist");
    check(flock(lock_a, LOCK_EX | LOCK_NB) == -1 && errno == EAGAIN, "and keep an exclusive one out");
    close(lock_b);
    check(flock(lock_a, LOCK_EX | LOCK_NB) == 0, "closing a file releases its lock");
    const pid_t waiter = fork();
    if (waiter == 0) {
        const int fd3 = open("/tmp/ipctest-lock", O_RDWR);
        _exit(flock(fd3, LOCK_EX) == 0 ? 0 : 1);   // waits for the parent
    }
    usleep(30000);
    check(flock(lock_a, LOCK_UN) == 0 && reap(waiter) == 0, "a blocked flock is granted when the holder unlocks");
    close(lock_a); close(lock_a2);
    unlink("/tmp/ipctest-lock");

    std::printf("[ipctest] descriptors: close-on-exec across exec, F_DUPFD, unlink while open, flock\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "check-fds") == 0)
        return check_fds(std::atoi(argv[2]), std::atoi(argv[3]));

    std::printf("[ipctest] pid %d\n", static_cast<int>(getpid()));
    test_signals();
    test_pipes();
    test_poll();
    test_sockets();
    test_shared_buffer();
    test_epoll();
    test_descriptors();

    if (g_failures == 0) {
        std::printf("[ipctest] all %d checks passed\n", g_checks);
        std::printf("[ipctest] M6 exit test passed\n");
        return 0;
    }
    std::printf("[ipctest] M6 exit test FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
