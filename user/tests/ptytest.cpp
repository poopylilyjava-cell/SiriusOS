// ptytest: pseudo-terminals, and the sessions and process groups they work
// with -- what a terminal emulator (weston-terminal, M8) needs of the kernel:
// forkpty() a shell, send it what is typed, show what it prints, and Ctrl+C.
//
// Prints one line per failed check and exits 0 only if all of them pass.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    g_checks++;
    if (condition) return;
    g_failures++;
    std::printf("[ptytest] FAILED: %s (errno %d)\n", what, errno);
}

// Everything readable from `fd` within `ms` milliseconds of quiet.
std::string drain(int fd, int ms = 100) {
    std::string out;
    for (;;) {
        pollfd p{ fd, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0 || !(p.revents & POLLIN)) return out;
        char buffer[256];
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) return out;
        out.append(buffer, (size_t)n);
    }
}

void put(int fd, const char* text) {
    const ssize_t n = write(fd, text, std::strlen(text));
    (void)n;
}

void test_line_discipline() {
    int master, slave;
    check(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0, "openpty makes a pair");
    check(isatty(master) && isatty(slave), "both ends are terminals");
    termios t{};
    check(tcgetattr(slave, &t) == 0 && (t.c_lflag & ICANON) && (t.c_lflag & ECHO) && (t.c_oflag & ONLCR),
          "a new one starts canonical, echoing, with newline translation");

    // Typed with a carriage return, as a keyboard sends Enter.
    put(master, "hello\r");
    char line[64] = {};
    check(read(slave, line, sizeof(line)) == 6 && std::strcmp(line, "hello\n") == 0, "a line typed is read as a line, CR made LF");
    check(drain(master) == "hello\r\n", "and echoed back, with CR LF");

    put(master, "abX\x7f" "c\r");
    std::memset(line, 0, sizeof(line));
    check(read(slave, line, sizeof(line)) == 4 && std::strcmp(line, "abc\n") == 0, "Backspace erases before the line is read");
    check(drain(master) == "abX\b \bc\r\n", "and rubs out its echo");

    put(master, "one\rtwo\r");
    std::memset(line, 0, sizeof(line));
    check(read(slave, line, sizeof(line)) == 4 && std::strcmp(line, "one\n") == 0, "a read returns one line at a time");
    std::memset(line, 0, sizeof(line));
    check(read(slave, line, sizeof(line)) == 4 && std::strcmp(line, "two\n") == 0, "and the next one next");
    drain(master);

    put(slave, "out\nput");
    check(drain(master) == "out\r\nput", "program output has its newlines made CR LF");

    put(master, "\x04");
    check(read(slave, line, sizeof(line)) == 0, "Ctrl+D on an empty line is end of file");

    int ready = -1;
    put(master, "partial");
    check(ioctl(slave, FIONREAD, &ready) == 0 && ready == 0, "an unfinished line is not readable yet");
    put(master, "\x15");   // Ctrl+U
    put(master, "x\r");
    std::memset(line, 0, sizeof(line));
    check(read(slave, line, sizeof(line)) == 2 && std::strcmp(line, "x\n") == 0, "Ctrl+U kills the line typed so far");
    drain(master);

    // Raw: what a full-screen program asks for.
    termios raw = t;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    check(tcsetattr(slave, TCSANOW, &raw) == 0, "tcsetattr");
    put(master, "q\r");
    std::memset(line, 0, sizeof(line));
    check(read(slave, line, sizeof(line)) == 2 && line[0] == 'q' && line[1] == '\r', "raw input arrives at once, untranslated");
    check(drain(master, 50).empty(), "and is not echoed");
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(slave, TCSANOW, &raw);
    check(read(slave, line, sizeof(line)) == 0, "VTIME: a read with nothing to read times out");
    tcsetattr(slave, TCSANOW, &t);

    winsize size{ 30, 100, 0, 0 };
    winsize seen{};
    check(ioctl(master, TIOCSWINSZ, &size) == 0 && ioctl(slave, TIOCGWINSZ, &seen) == 0 && seen.ws_row == 30 && seen.ws_col == 100,
          "the window size set on the master is the slave's");

    close(slave);
    pollfd p{ master, POLLIN, 0 };
    check(poll(&p, 1, 100) == 1 && (p.revents & POLLHUP), "the master hangs up once the slave is closed");
    errno = 0;
    check(read(master, line, sizeof(line)) == -1 && errno == EIO, "and reads fail with EIO");
    close(master);
}

void test_forkpty() {
    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, nullptr);
    if (child == 0) {
        // A session of its own, with the terminal as its controlling one
        // and stdin, stdout and stderr.
        int result = 0;
        if (getsid(0) != getpid()) result |= 1;
        if (!isatty(0) || !isatty(1) || !isatty(2)) result |= 2;
        if (tcgetpgrp(0) != getpgrp()) result |= 4;
        std::printf("ready %d\n", result);
        std::fflush(stdout);
        char buffer[16];
        for (;;) {                       // until Ctrl+C ends it
            if (read(0, buffer, sizeof(buffer)) <= 0) _exit(10);
        }
    }
    check(child > 0 && master >= 0, "forkpty");
    if (child <= 0) return;
    const std::string greeting = drain(master, 1000);
    check(greeting == "ready 0\r\n", "the child is a session leader with the terminal on stdin, stdout and stderr");
    if (greeting != "ready 0\r\n") std::printf("[ptytest]   it said: %s\n", greeting.c_str());

    put(master, "\x03");   // Ctrl+C
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT,
          "Ctrl+C typed on the master sends SIGINT to the foreground group");
    check(drain(master) == "^C", "and is echoed as ^C");
    pollfd p{ master, POLLIN, 0 };
    check(poll(&p, 1, 1000) == 1 && (p.revents & POLLHUP), "the child gone, the master hangs up");
    close(master);
}

void test_groups() {
    // setsid() makes a session of a process that does not lead a group,
    // and is refused to one that does.
    const pid_t first = fork();
    if (first == 0) {
        if (setsid() != getpid() || getsid(0) != getpid() || getpgrp() != getpid()) _exit(1);
        if (setsid() != -1 || errno != EPERM) _exit(2);
        _exit(0);
    }
    int first_status = -1;
    waitpid(first, &first_status, 0);
    check(WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0, "setsid makes a new session, once");

    const pid_t child = fork();
    if (child == 0) {
        if (setpgid(0, 0) != 0) _exit(1);
        if (getpgrp() != getpid()) _exit(2);
        pause();
        _exit(3);
    }
    // Until the child has made its group, there is nothing to signal.
    for (int i = 0; i < 100 && getpgid(child) != child; i++) usleep(10000);
    check(getpgid(child) == child, "setpgid(0, 0) makes a new group");
    check(kill(-child, SIGTERM) == 0, "kill(-group) finds it");
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "and signals its members");
    errno = 0;
    check(kill(-child, SIGTERM) == -1 && errno == ESRCH, "an empty group is not there");
}

} // namespace

int main() {
    std::printf("[ptytest] pid %d\n", static_cast<int>(getpid()));
    test_line_discipline();
    test_forkpty();
    test_groups();
    if (g_failures == 0) {
        std::printf("[ptytest] all %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("[ptytest] FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
