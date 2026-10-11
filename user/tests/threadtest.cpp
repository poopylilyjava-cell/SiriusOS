// threadtest: threads, from inside. Several flows of control in one address
// space, with the same files and the same signal handlers; what the kernel
// has to get right is everything they can do to each other.
//
// Prints one line per group of checks and exits 0 only if all of them pass.
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
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
    std::printf("[threadtest] FAILED: %s (errno %d)\n", what, errno);
}

long milliseconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void nap(long ms) {
    timespec ts{ ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, nullptr);
}

// --- create and join -------------------------------------------------------------------

void* twice(void* argument) { return reinterpret_cast<void*>(reinterpret_cast<long>(argument) * 2); }

void test_create_join() {
    pthread_t threads[8];
    for (long i = 0; i < 8; i++)
        check(pthread_create(&threads[i], nullptr, twice, reinterpret_cast<void*>(i + 1)) == 0, "pthread_create");
    long sum = 0;
    for (auto& thread : threads) {
        void* result = nullptr;
        check(pthread_join(thread, &result) == 0, "pthread_join");
        sum += reinterpret_cast<long>(result);
    }
    check(sum == 72, "eight threads each returned twice their argument");
    std::printf("[threadtest] create and join: 8 threads, results sum to %ld\n", sum);
}

// --- the C++ library's own -----------------------------------------------------------------

void test_std_thread() {
    std::mutex mutex;
    long total = 0;
    std::vector<std::thread> threads;
    for (long i = 1; i <= 4; i++)
        threads.emplace_back([&, i] {
            for (int round = 0; round < 1000; round++) {
                std::lock_guard<std::mutex> guard(mutex);
                total += i;
            }
        });
    for (auto& thread : threads) thread.join();
    check(total == 10000, "std::thread and std::mutex: four threads add up");
    std::printf("[threadtest] std::thread: 4 threads under a std::mutex, total %ld\n", total);
}

// --- a mutex -----------------------------------------------------------------------------

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
long g_counter = 0;
constexpr long kRounds = 50000;

void* count(void*) {
    for (long i = 0; i < kRounds; i++) {
        pthread_mutex_lock(&g_mutex);
        // Read, give the scheduler every chance to interfere, write.
        const long seen = g_counter;
        if (i % 4096 == 0) sched_yield();
        g_counter = seen + 1;
        pthread_mutex_unlock(&g_mutex);
    }
    return nullptr;
}

void test_mutex() {
    pthread_t threads[4];
    for (auto& thread : threads) pthread_create(&thread, nullptr, count, nullptr);
    for (auto& thread : threads) pthread_join(thread, nullptr);
    check(g_counter == 4 * kRounds, "a mutex makes four threads' increments add up");
    std::printf("[threadtest] mutex: %ld increments by 4 threads, none lost\n", g_counter);
}

// --- a condition variable ------------------------------------------------------------------

pthread_cond_t g_condition = PTHREAD_COND_INITIALIZER;
pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_queued = 0;       // items waiting
long g_consumed = 0;
bool g_closed = false;

void* consume(void*) {
    pthread_mutex_lock(&g_queue_mutex);
    for (;;) {
        while (g_queued == 0 && !g_closed) pthread_cond_wait(&g_condition, &g_queue_mutex);
        if (g_queued == 0) break;
        g_queued--;
        g_consumed++;
    }
    pthread_mutex_unlock(&g_queue_mutex);
    return nullptr;
}

void test_condition() {
    pthread_t consumers[3];
    for (auto& thread : consumers) pthread_create(&thread, nullptr, consume, nullptr);
    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&g_queue_mutex);
        g_queued++;
        pthread_mutex_unlock(&g_queue_mutex);
        pthread_cond_signal(&g_condition);
        if (i % 50 == 0) nap(1);   // let them catch up and go back to sleep
    }
    pthread_mutex_lock(&g_queue_mutex);
    g_closed = true;
    pthread_mutex_unlock(&g_queue_mutex);
    pthread_cond_broadcast(&g_condition);
    for (auto& thread : consumers) pthread_join(thread, nullptr);
    check(g_consumed == 300 && g_queued == 0, "three consumers took all 300 items off a condition variable");

    // Nobody signals this one: the wait must give up by itself.
    pthread_mutex_lock(&g_queue_mutex);
    timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 60 * 1000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    const long before = milliseconds();
    const int status = pthread_cond_timedwait(&g_condition, &g_queue_mutex, &deadline);
    const long waited = milliseconds() - before;
    pthread_mutex_unlock(&g_queue_mutex);
    check(status == ETIMEDOUT, "a timed wait nobody signals times out");
    check(waited >= 50 && waited < 1000, "and takes about as long as it was given");
    std::printf("[threadtest] condition variable: %ld items consumed; a timed wait gave up after %ld ms\n", g_consumed, waited);
}

// --- thread-local storage ---------------------------------------------------------------------

thread_local long t_value = 7;

void* own_storage(void* argument) {
    const long id = reinterpret_cast<long>(argument);
    bool ok = t_value == 7;   // a fresh copy, whatever the others did to theirs
    for (int i = 0; i < 200 && ok; i++) {
        t_value = id * 1000 + i;
        errno = static_cast<int>(id);
        sched_yield();
        ok = t_value == id * 1000 + i && errno == static_cast<int>(id);
    }
    return reinterpret_cast<void*>(static_cast<long>(ok));
}

void test_thread_local() {
    t_value = 99;
    pthread_t threads[4];
    for (long i = 0; i < 4; i++) pthread_create(&threads[i], nullptr, own_storage, reinterpret_cast<void*>(i + 1));
    bool all = true;
    for (auto& thread : threads) {
        void* result = nullptr;
        pthread_join(thread, &result);
        all = all && result != nullptr;
    }
    check(all, "each thread has its own thread-local variables and errno");
    check(t_value == 99, "and the main thread's are untouched");
    std::printf("[threadtest] thread-local storage: 4 threads, each with its own\n");
}

// --- shared files, shared memory ---------------------------------------------------------------

int g_pipe[2];
long g_bytes_read = 0;

void* read_until_end(void*) {
    char buffer[64];
    for (;;) {
        const ssize_t n = read(g_pipe[0], buffer, sizeof(buffer));
        if (n <= 0) return reinterpret_cast<void*>(static_cast<long>(n));
        g_bytes_read += n;
    }
}

void* allocate(void*) {
    // The heap is one lock and one address space, shared.
    for (int i = 0; i < 400; i++) {
        const size_t size = 64 + static_cast<size_t>(i) * 300;
        auto* p = static_cast<unsigned char*>(std::malloc(size));
        if (p == nullptr) return nullptr;
        std::memset(p, i, size);
        if (p[size - 1] != static_cast<unsigned char>(i)) return nullptr;
        std::free(p);
        void* page = mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return nullptr;
        static_cast<char*>(page)[4096] = 1;
        munmap(page, 8192);
    }
    return reinterpret_cast<void*>(1L);
}

void test_shared() {
    check(pipe(g_pipe) == 0, "pipe");
    pthread_t reader;
    pthread_create(&reader, nullptr, read_until_end, nullptr);
    nap(20);   // it is asleep in read() now
    check(write(g_pipe[1], "hello", 5) == 5, "write to a pipe another thread is reading");
    nap(20);
    // Closing the descriptor under a thread that is blocked on it must not
    // pull the file out from under that thread.
    const int read_end = g_pipe[0];
    check(close(read_end) == 0, "close a descriptor another thread is blocked reading");
    check(write(g_pipe[1], "world", 5) == 5, "the pipe still has its reader");
    close(g_pipe[1]);
    void* result = nullptr;
    pthread_join(reader, &result);
    // The read that was under way finished; the next one found no descriptor.
    check(g_bytes_read == 10, "the reader got everything written, across the close");
    check(reinterpret_cast<long>(result) == -1, "and then an error for the descriptor that is gone");

    pthread_t threads[4];
    for (auto& thread : threads) pthread_create(&thread, nullptr, allocate, nullptr);
    bool all = true;
    for (auto& thread : threads) {
        void* ok = nullptr;
        pthread_join(thread, &ok);
        all = all && ok != nullptr;
    }
    check(all, "four threads allocating, mapping and unmapping at once");
    std::printf("[threadtest] shared files and memory: a reader survived its descriptor being closed, %ld bytes read\n", g_bytes_read);
}

// --- signals ---------------------------------------------------------------------------------------

thread_local int t_name = 0;
std::atomic<int> g_handled_by{0};
std::atomic<int> g_handled{0};
std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_handled_by = t_name;
    g_handled++;
}

void* wait_for_stop(void* argument) {
    t_name = static_cast<int>(reinterpret_cast<long>(argument));
    while (!g_stop) nap(2);
    return nullptr;
}

void test_signals() {
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigaction(SIGUSR1, &action, nullptr);
    sigaction(SIGUSR2, &action, nullptr);
    t_name = 1;

    pthread_t second, third;
    pthread_create(&second, nullptr, wait_for_stop, reinterpret_cast<void*>(2L));
    pthread_create(&third, nullptr, wait_for_stop, reinterpret_cast<void*>(3L));
    nap(10);

    // A signal for one thread is that thread's.
    check(pthread_kill(third, SIGUSR1) == 0, "pthread_kill");
    for (int i = 0; i < 200 && g_handled == 0; i++) nap(2);
    check(g_handled == 1 && g_handled_by == 3, "a signal sent to a thread runs its handler on that thread");

    // A signal for the process goes to a thread that does not block it. The
    // mask is each thread's own: blocking it here leaves it open there.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    g_handled_by = 0;
    kill(getpid(), SIGUSR2);
    for (int i = 0; i < 200 && g_handled == 1; i++) nap(2);
    check(g_handled == 2 && g_handled_by != 1 && g_handled_by != 0, "a signal for the process is taken by a thread that has not blocked it");
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

    g_stop = true;
    pthread_join(second, nullptr);
    pthread_join(third, nullptr);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    std::printf("[threadtest] signals: one delivered to a chosen thread, one to a thread that had not blocked it\n");
}

// --- the end of a process with threads in it --------------------------------------------------------

void* spin(void*) { for (volatile unsigned long i = 0;; i = i + 1) {} }
void* sleep_in_read(void* argument) {
    char c;
    read(*static_cast<int*>(argument), &c, 1);
    return nullptr;
}
void* sleep_on_lock(void* argument) {
    pthread_mutex_lock(static_cast<pthread_mutex_t*>(argument));
    return nullptr;
}
void* exit_from_here(void*) {
    nap(20);
    _exit(42);
}
void* return_soon(void*) {
    nap(30);
    return nullptr;
}

// A child with threads in every state a thread can be in: running, asleep in
// a system call, asleep on a lock.
void populate(int* fds, pthread_mutex_t* held) {
    pthread_t thread;
    pipe(fds);
    pthread_mutex_lock(held);
    pthread_create(&thread, nullptr, spin, nullptr);
    pthread_create(&thread, nullptr, sleep_in_read, &fds[0]);
    pthread_create(&thread, nullptr, sleep_on_lock, held);
}

int status_of_child(pid_t pid) {
    int status = -1;
    const long deadline = milliseconds() + 10000;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return status;
        if (done < 0 || milliseconds() > deadline) { kill(pid, SIGKILL); return -1; }
        nap(5);
    }
}

void test_endings(const char* self) {
    static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
    int fds[2];

    // exit() from one thread ends all of them.
    pid_t pid = fork();
    if (pid == 0) {
        populate(fds, &held);
        pthread_t thread;
        pthread_create(&thread, nullptr, exit_from_here, nullptr);
        for (;;) pause();
    }
    int status = status_of_child(pid);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 42, "exit from one thread ends a process whose others run, read and wait");

    // So does a signal nobody handles.
    pid = fork();
    if (pid == 0) {
        populate(fds, &held);
        for (;;) pause();
    }
    nap(30);
    kill(pid, SIGTERM);
    status = status_of_child(pid);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "a fatal signal ends every thread");

    // The main thread may leave first: the process lasts as long as its last thread.
    pid = fork();
    if (pid == 0) {
        pthread_t thread;
        pthread_create(&thread, nullptr, return_soon, nullptr);
        pthread_exit(nullptr);
    }
    status = status_of_child(pid);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a process outlives its main thread and ends with its last, status 0");

    // fork copies the calling thread only.
    pthread_t bystander;
    g_stop = false;
    pthread_create(&bystander, nullptr, wait_for_stop, reinterpret_cast<void*>(9L));
    pid = fork();
    if (pid == 0) {
        // If the bystander had come along, it would still be here to see
        // the flag change; nothing is, and the child's exit is its own.
        nap(10);
        _exit(7);
    }
    status = status_of_child(pid);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 7, "fork from a threaded process gives a child that runs and exits");
    g_stop = true;
    pthread_join(bystander, nullptr);

    // exec replaces all the threads with the new program's one.
    pid = fork();
    if (pid == 0) {
        populate(fds, &held);
        nap(20);
        execl(self, "threadtest", "--exec-child", static_cast<char*>(nullptr));
        _exit(1);
    }
    status = status_of_child(pid);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 9, "exec from a threaded process starts the new program alone");
    std::printf("[threadtest] endings: exit, a fatal signal, the last thread returning, fork and exec\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--exec-child") == 0) return 9;
    setvbuf(stdout, nullptr, _IOLBF, 0);

    test_create_join();
    test_std_thread();
    test_mutex();
    test_condition();
    test_thread_local();
    test_shared();
    test_signals();
    test_endings("/bin/threadtest");

    if (g_failures) {
        std::printf("[threadtest] %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("[threadtest] %d checks passed\n", g_checks);
    std::printf("[threadtest] threads test passed\n");
    return 0;
}
