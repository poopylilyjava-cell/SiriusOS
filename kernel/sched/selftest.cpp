// In-kernel tests for threads, the scheduler and the locks.
//
// Like the memory tests these need the real machine: preemption, the per-CPU
// run queues and cross-CPU wakeups only exist with real timer interrupts on
// real (virtual) cores.
#include <sched/selftest.hpp>
#include <sched/mutex.hpp>
#include <sched/sched.hpp>
#include <arch/x86_64/percpu.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <klib/print.hpp>

namespace sched {
namespace {

u32 g_checks = 0;
u32 g_failures = 0;

void check(bool condition, const char* what) {
    g_checks++;
    if (condition) return;
    g_failures++;
    LOG_ERROR("selftest FAILED: %s", what);
}

// Wait, sleeping, until `done` reaches `target` or the timeout passes.
bool wait_for(const u32& done, u32 target, u64 timeout_ms) {
    const u64 deadline = uptime_ms() + timeout_ms;
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < target) {
        if (uptime_ms() > deadline) return false;
        sleep_ms(10);
    }
    return true;
}

void test_sleep() {
    LOG_INFO("selftest: sleep");
    const u64 before = uptime_ms();
    sleep_ms(100);
    const u64 slept = uptime_ms() - before;
    check(slept >= 100, "sleep_ms(100) sleeps at least 100 ms");
    // Generous: TCG on a loaded CI runner can be slow, but not this slow.
    check(slept < 1000, "sleep_ms(100) wakes within a second");
}

// --- threads run, and on more than one CPU ------------------------------------
struct SpreadState {
    u32 done;
    u64 cpus_seen;  // bit i set once something ran on cpu i
};

void spread_worker(void* arg) {
    auto* state = static_cast<SpreadState*>(arg);
    __atomic_fetch_or(&state->cpus_seen, 1ULL << percpu::current()->index, __ATOMIC_RELAXED);
    __atomic_fetch_add(&state->done, 1, __ATOMIC_RELEASE);
}

void test_threads_spread() {
    LOG_INFO("selftest: threads spread across cpus");
    const u32 cpus = percpu::count();
    const u32 threads = cpus * 2;

    SpreadState state{};
    for (u32 i = 0; i < threads; i++)
        check(spawn("spread", spread_worker, &state) != nullptr, "spawning a kernel thread succeeds");

    check(wait_for(state.done, threads, 5000), "every spawned thread runs to completion");
    const u32 used = static_cast<u32>(__builtin_popcountll(state.cpus_seen));
    check(used == cpus, "threads ran on every online cpu");
    LOG_INFO("selftest: %u threads ran on %u of %u cpus", threads, used, cpus);
}

// --- mutual exclusion under contention, across CPUs -----------------------
struct MutexState {
    Mutex mutex;
    u64   counter;     // deliberately not atomic: the mutex is all that guards it
    u32   in_critical; // must never be seen above 1
    u32   violations;
    u32   done;
};

constexpr u32 kMutexWorkers = 8;
constexpr u32 kMutexIterations = 2000;

void mutex_worker(void* arg) {
    auto* state = static_cast<MutexState*>(arg);
    for (u32 i = 0; i < kMutexIterations; i++) {
        ScopedMutex guard(state->mutex);
        if (__atomic_add_fetch(&state->in_critical, 1, __ATOMIC_ACQ_REL) != 1)
            __atomic_fetch_add(&state->violations, 1, __ATOMIC_RELAXED);

        // A read-modify-write spread out over a reschedule now and then, so
        // that without the mutex other threads would interleave with it.
        const u64 value = state->counter;
        if ((i & 63) == 0) yield();
        state->counter = value + 1;

        __atomic_sub_fetch(&state->in_critical, 1, __ATOMIC_ACQ_REL);
    }
    __atomic_fetch_add(&state->done, 1, __ATOMIC_RELEASE);
}

void test_mutex() {
    LOG_INFO("selftest: mutex under contention (%u threads x %u)", kMutexWorkers, kMutexIterations);
    auto* state = new MutexState{};
    for (u32 i = 0; i < kMutexWorkers; i++)
        check(spawn("mutex", mutex_worker, state) != nullptr, "spawning a mutex worker succeeds");

    const u64 started = uptime_ms();
    const bool finished = wait_for(state->done, kMutexWorkers, 30000);
    check(finished, "every mutex worker finishes");
    LOG_INFO("selftest: mutex workers %s after %lu ms: %u of %u done, counter %lu",
             finished ? "finished" : "STALLED", uptime_ms() - started,
             __atomic_load_n(&state->done, __ATOMIC_RELAXED), kMutexWorkers, state->counter);
    check(state->violations == 0, "no two threads were ever inside the mutex together");
    check(state->counter == static_cast<u64>(kMutexWorkers) * kMutexIterations,
          "no increment was lost under the mutex");
    delete state;
}

// --- preemption: a thread that never yields still lets others run ---------
struct PreemptState {
    u32 stop;
    u32 spinner_running;
    u32 others_ran;
};

void spinner(void* arg) {
    auto* state = static_cast<PreemptState*>(arg);
    __atomic_store_n(&state->spinner_running, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&state->stop, __ATOMIC_ACQUIRE)) cpu_relax();
}

void bystander(void* arg) {
    auto* state = static_cast<PreemptState*>(arg);
    __atomic_fetch_add(&state->others_ran, 1, __ATOMIC_RELEASE);
}

void test_preemption() {
    LOG_INFO("selftest: preemption");
    PreemptState state{};
    // Both on this CPU: the bystander can only run if the spinner is preempted.
    Cpu* here = percpu::current();
    check(spawn("spinner", spinner, &state, nullptr, here) != nullptr, "spawning the spinner succeeds");
    yield();
    check(spawn("bystander", bystander, &state, nullptr, here) != nullptr, "spawning the bystander succeeds");

    check(wait_for(state.others_ran, 1, 5000), "a busy-looping thread is preempted");
    __atomic_store_n(&state.stop, 1, __ATOMIC_RELEASE);
    check(__atomic_load_n(&state.spinner_running, __ATOMIC_ACQUIRE) == 1, "the spinner did run");
    sleep_ms(50);  // let the spinner see `stop` and exit before `state` goes away
}

// Dead threads must give back their stacks and records. Reaping happens on
// the dead thread's own CPU at its next switch, so allow it a moment.
void test_reaping(u64 pages_before, u64 objects_before) {
    LOG_INFO("selftest: thread reaping");
    const u64 deadline = uptime_ms() + 2000;
    while (uptime_ms() < deadline) {
        if (pmm::stats().free_pages == pages_before && heap::stats().live_objects == objects_before) break;
        sleep_ms(20);
    }
    check(pmm::stats().free_pages == pages_before, "exited threads return every page");
    check(heap::stats().live_objects == objects_before, "exited threads return every heap object");
    const Stats s = stats();
    check(s.threads_created == s.threads_reaped + 1, "every thread but this one has been reaped");
}

} // namespace

bool run_selftests() {
    LOG_INFO("selftest: starting scheduler tests on %u cpus", percpu::count());
    const u64 pages_before = pmm::stats().free_pages;
    const u64 objects_before = heap::stats().live_objects;

    test_sleep();
    test_threads_spread();
    test_mutex();
    test_preemption();
    test_reaping(pages_before, objects_before);

    const Stats s = stats();
    LOG_INFO("selftest: %lu context switches, %lu threads created", s.switches, s.threads_created);

    if (g_failures == 0) {
        LOG_INFO("selftest: all %u scheduler checks passed", g_checks);
        return true;
    }
    LOG_ERROR("selftest: %u of %u scheduler checks FAILED", g_failures, g_checks);
    return false;
}

} // namespace sched
