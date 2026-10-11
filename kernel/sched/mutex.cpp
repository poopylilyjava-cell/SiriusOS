#include <sched/mutex.hpp>
#include <sched/sched.hpp>
#include <klib/panic.hpp>

namespace sched {

void Mutex::lock() {
    Thread* self = current();
    const u64 flags = m_lock.lock_irqsave();

    if (m_owner == nullptr) {
        m_owner = self;
        m_lock.unlock_irqrestore(flags);
        return;
    }
    if (m_owner == self) panic("mutex %p: recursive lock by thread %u", this, self->tid);

    self->next = nullptr;
    if (m_waiters_tail) m_waiters_tail->next = self;
    else                m_waiters_head = self;
    m_waiters_tail = self;

    // Sleeps until unlock() names us the owner. block_current() drops m_lock.
    block_current(m_lock, flags);

    if (m_owner != self) panic("mutex %p: thread %u woke without ownership", this, self->tid);
}

bool Mutex::try_lock() {
    const u64 flags = m_lock.lock_irqsave();
    const bool acquired = (m_owner == nullptr);
    if (acquired) m_owner = current();
    m_lock.unlock_irqrestore(flags);
    return acquired;
}

void Mutex::unlock() {
    const u64 flags = m_lock.lock_irqsave();
    if (m_owner != current()) panic("mutex %p: unlocked by a thread that does not own it", this);

    Thread* next = m_waiters_head;
    if (next) {
        m_waiters_head = next->next;
        if (m_waiters_head == nullptr) m_waiters_tail = nullptr;
        next->next = nullptr;
    }
    m_owner = next;
    // Still under m_lock: the waiter cannot be woken before it is fully
    // blocked, because blocking holds its run queue lock until it is off-CPU.
    if (next) wake(next);

    m_lock.unlock_irqrestore(flags);
}

bool Mutex::held_by_current() const { return __atomic_load_n(&m_owner, __ATOMIC_RELAXED) == current(); }

} // namespace sched
