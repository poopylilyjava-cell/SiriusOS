#pragma once
#include <sched/thread.hpp>

namespace sched {

// A sleeping lock: a contended lock() gives the CPU away instead of spinning.
// For critical sections that may be long or may block. Never take one with
// interrupts disabled or from an interrupt handler -- use a Spinlock there.
//
// Unlock hands ownership straight to the first waiter, so waiters are served
// in arrival order and a thread that keeps re-locking cannot starve the rest.
class Mutex {
public:
    void lock();
    bool try_lock();
    void unlock();
    bool held_by_current() const;

private:
    Spinlock m_lock;           // guards the fields below
    Thread*  m_owner = nullptr;
    Thread*  m_waiters_head = nullptr;
    Thread*  m_waiters_tail = nullptr;
};

class ScopedMutex {
public:
    explicit ScopedMutex(Mutex& mutex) : m_mutex(mutex) { m_mutex.lock(); }
    ~ScopedMutex() { m_mutex.unlock(); }
    ScopedMutex(const ScopedMutex&) = delete;
    ScopedMutex& operator=(const ScopedMutex&) = delete;

private:
    Mutex& m_mutex;
};

} // namespace sched
