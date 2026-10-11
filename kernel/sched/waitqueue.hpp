#pragma once
#include <sched/thread.hpp>

namespace sched {

// Threads waiting for a condition that some other thread will announce.
//
// The condition lives under a Spinlock the caller owns. The pattern is:
//
//     u64 flags = lock.lock_irqsave();
//     while (!condition) {
//         queue.wait(lock, flags);           // sleeps; returns with lock released
//         flags = lock.lock_irqsave();
//     }
//     ... condition holds, lock held ...
//     lock.unlock_irqrestore(flags);
//
// and the announcer changes the condition and calls wake_all() under the same
// lock. Because wait() releases the lock only once the thread is safely
// blocked, a wake between the check and the sleep cannot be lost.
class WaitQueue {
public:
    // Caller holds `lock`, taken with lock_irqsave() returning `flags`.
    void wait(Spinlock& lock, u64 flags);
    // Caller holds the lock guarding the condition.
    void wake_all();
    bool empty() const { return m_head == nullptr; }

private:
    Thread* m_head = nullptr;
    Thread* m_tail = nullptr;
};

} // namespace sched
