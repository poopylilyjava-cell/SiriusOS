#include <sched/events.hpp>
#include <sched/process.hpp>
#include <sched/sched.hpp>
#include <sched/signal.hpp>
#include <sched/waitqueue.hpp>
#include <arch/x86_64/clock.hpp>
#include <klib/spinlock.hpp>
#include <sirius/errno.h>

namespace events {
namespace {

constexpr u64 kNoDeadline = ~0ULL;

Spinlock g_lock;
u64 g_generation = 0;
sched::WaitQueue g_sleepers;
u64 g_earliest_deadline = kNoDeadline;   // among the sleepers; they re-register when woken

} // namespace

u64 snapshot() { return __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE); }

void notify() {
    ScopedLock guard(g_lock);
    __atomic_store_n(&g_generation, g_generation + 1, __ATOMIC_RELEASE);
    g_earliest_deadline = kNoDeadline;
    g_sleepers.wake_all();
}

int wait(u64 snapshot, u64 deadline_ns) {
    u64 flags = g_lock.lock_irqsave();
    if (g_generation != snapshot) { g_lock.unlock_irqrestore(flags); return 0; }
    // Posting a signal notifies, and that needs this lock: one posted after
    // this check finds the thread already asleep, and wakes it.
    // So does ending the process from another of its threads.
    const sched::Thread* self = sched::current();
    if (self->process && signal::deliverable(self)) { g_lock.unlock_irqrestore(flags); return -SIRIUS_EINTR; }
    if (deadline_ns) {
        if (clock::now_ns() >= deadline_ns) { g_lock.unlock_irqrestore(flags); return -SIRIUS_ETIMEDOUT; }
        if (deadline_ns < g_earliest_deadline) g_earliest_deadline = deadline_ns;
    }
    g_sleepers.wait(g_lock, flags);
    return 0;
}

void tick() {
    // Read without the lock: a stale value costs one tick of delay at worst.
    const u64 earliest = __atomic_load_n(&g_earliest_deadline, __ATOMIC_RELAXED);
    if (earliest != kNoDeadline && clock::now_ns() >= earliest) notify();
}

} // namespace events
