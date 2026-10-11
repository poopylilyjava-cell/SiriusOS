#include <klib/spinlock.hpp>
#include <klib/panic.hpp>

// Long enough that no legitimate critical section gets near it (minutes of
// contention under TCG would be a bug in itself), short enough that a deadlock
// becomes a backtrace instead of a hang.
constexpr u64 kDeadlockSpins = 1ULL << 32;

void Spinlock::lock() {
    for (u64 spins = 0;; spins++) {
        if (try_lock()) return;
        while (is_locked()) {
            cpu_relax();
            if (++spins == kDeadlockSpins) panic("spinlock %p: probable deadlock", this);
        }
    }
}
