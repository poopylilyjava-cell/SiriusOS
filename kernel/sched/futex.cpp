// futex: the one thing threads need from the kernel to build locks -- a way
// to sleep until another thread says a word in memory has changed.
//
// The word belongs to the program. A thread that finds it in the state it
// must wait out calls futex_wait with the value it saw; a thread that changes
// it calls futex_wake. The kernel only has to make sure a wake that comes
// between the waiter's look and its sleep is not lost, which it does by
// putting the waiter on the list *before* reading the word: a waker that has
// already changed it is seen in the value, and one that has not finds the
// waiter.
//
// Waiters are matched by process and address, so a futex in memory shared
// between processes does not work across them. Sleeping is on the event
// channel like everything else (sched/events.hpp).
#include <sched/process.hpp>
#include <sched/events.hpp>
#include <sched/sched.hpp>
#include <arch/x86_64/clock.hpp>
#include <mm/usercopy.hpp>
#include <sirius/errno.h>

namespace sched {
namespace {

// Lives on its thread's kernel stack while that thread is in futex_wait.
struct Waiter {
    const Process* process;
    u64 address;
    bool woken;      // set, and the waiter unlinked, by futex_wake
    Waiter* next;
};

Spinlock g_lock;
Waiter* g_head = nullptr;
Waiter* g_tail = nullptr;   // woken in the order they came

// Caller holds g_lock.
void unlink(Waiter* waiter) {
    Waiter* previous = nullptr;
    for (Waiter* w = g_head; w; previous = w, w = w->next) {
        if (w != waiter) continue;
        (previous ? previous->next : g_head) = w->next;
        if (g_tail == w) g_tail = previous;
        return;
    }
}

// Off the list, unless a wake got there first: was it woken?
bool leave(Waiter* waiter) {
    ScopedLock guard(g_lock);
    if (!waiter->woken) unlink(waiter);
    return waiter->woken;
}

} // namespace

i64 futex_wait(u64 address, u64 expected, u64 timeout_ns) {
    if (address % sizeof(u32)) return -SIRIUS_EINVAL;
    const u64 deadline = timeout_ns == ~0ULL ? 0 : clock::now_ns() + timeout_ns + 1;   // 0 would mean none

    Waiter waiter{ current_process(), address, false, nullptr };
    {
        ScopedLock guard(g_lock);
        (g_tail ? g_tail->next : g_head) = &waiter;
        g_tail = &waiter;
    }

    u32 value;
    if (const int status = usercopy::copy_from(&value, address, sizeof(value)); status < 0) { leave(&waiter); return status; }
    if (value != static_cast<u32>(expected)) { leave(&waiter); return -SIRIUS_EAGAIN; }

    for (;;) {
        const u64 snapshot = events::snapshot();
        {
            ScopedLock guard(g_lock);
            if (waiter.woken) return 0;
        }
        const int status = events::wait(snapshot, deadline);
        // A wake that raced with the timeout or the signal still counts: it
        // was spent on this thread and must not be lost.
        if (status < 0) return leave(&waiter) ? 0 : status;
    }
}

i64 futex_wake(u64 address, u64 count) {
    const Process* process = current_process();
    u64 woken = 0;
    {
        ScopedLock guard(g_lock);
        Waiter* previous = nullptr;
        for (Waiter* w = g_head; w && woken < count;) {
            Waiter* next = w->next;
            if (w->process == process && w->address == address) {
                (previous ? previous->next : g_head) = next;
                if (g_tail == w) g_tail = previous;
                w->woken = true;   // and never touched from here again: it is on a stack about to unwind
                woken++;
            } else {
                previous = w;
            }
            w = next;
        }
    }
    if (woken) events::notify();
    return static_cast<i64>(woken);
}

} // namespace sched
