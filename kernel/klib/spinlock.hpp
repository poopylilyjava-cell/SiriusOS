#pragma once
#include <klib/types.hpp>

// Interrupt state. Every spinlock in the kernel is taken with interrupts off:
// an interrupt handler that tries to take a lock its own CPU already holds
// spins forever, and in M2 the timer interrupt can reach the scheduler from
// anywhere.
namespace irq {

inline u64 save() {
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

inline void restore(u64 flags) {
    if (flags & (1 << 9)) __asm__ volatile("sti" : : : "memory");
}

inline bool enabled() {
    u64 flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1 << 9)) != 0;
}

} // namespace irq

// A test-and-test-and-set spinlock. lock()/unlock() do not touch the interrupt
// flag; callers use lock_irqsave() or ScopedLock unless interrupts are already
// known to be off.
class Spinlock {
public:
    void lock();
    bool try_lock() { return !__atomic_exchange_n(&m_locked, 1u, __ATOMIC_ACQUIRE); }
    void unlock()   { __atomic_store_n(&m_locked, 0u, __ATOMIC_RELEASE); }
    bool is_locked() const { return __atomic_load_n(&m_locked, __ATOMIC_RELAXED) != 0; }

    u64 lock_irqsave() {
        const u64 flags = irq::save();
        lock();
        return flags;
    }
    void unlock_irqrestore(u64 flags) {
        unlock();
        irq::restore(flags);
    }

private:
    u32 m_locked = 0;
};

class ScopedLock {
public:
    explicit ScopedLock(Spinlock& lock) : m_lock(lock), m_flags(lock.lock_irqsave()) {}
    ~ScopedLock() { m_lock.unlock_irqrestore(m_flags); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    Spinlock& m_lock;
    u64 m_flags;
};

inline void cpu_relax() { __asm__ volatile("pause" : : : "memory"); }
