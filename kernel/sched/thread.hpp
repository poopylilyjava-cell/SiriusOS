#pragma once
#include <klib/types.hpp>
#include <klib/spinlock.hpp>

struct Cpu;

namespace sched {

struct Process;
}
namespace vfs { struct File; }
namespace sched {

enum class ThreadState : u8 {
    Ready,     // on a run queue
    Running,   // current on its CPU
    Blocked,   // waiting for wake()
    Sleeping,  // on its CPU's sleeper list until a tick
    Dead,      // exited; reaped by the next switch on its CPU
};

// A kernel thread. User processes are kernel threads that drop to ring 3 and
// come back on interrupts and system calls, so they use this same record.
//
// A thread never moves between CPUs: it runs only on `cpu`, which keeps the
// run queues independent and the TLB rules simple. The threads of one process
// all share its CPU, so an address space is only ever loaded on one. Load
// balancing comes later, with TLB shootdowns.
struct Thread {
    u64 saved_rsp;            // valid while not running; see switch_context
    u64 kernel_stack_top;
    u64 kernel_stack_pages;   // 0 for a CPU's boot stack, which is never freed

    ThreadState state;
    u32 tid;
    Cpu* cpu;
    Process* process;         // null for pure kernel threads
    Thread* process_next;     // the process's other threads; under its threads_lock
    void* fpu_state;          // FXSAVE area, user threads only
    u64 fs_base;              // user threads only: the TLS pointer, set by tcb_set

    Thread* next;             // link in a run queue, sleeper list or wait queue
    u64 wake_tick;            // for Sleeping: the CPU tick to wake on
    u64 run_ticks;            // timer ticks that found this thread running: its CPU time

    // Signals (sched/signal.cpp). The mask is the thread's own. Pending here
    // are the signals sent to this thread in particular; those sent to the
    // process are in the Process, for whichever thread takes them first.
    u64  sig_blocked;
    u64  sig_pending;
    u64  sig_saved_mask;      // the mask to return to after sigsuspend's
    bool sig_has_saved_mask;

    // Open files this thread looked up during the system call it is in, each
    // holding a reference until the call returns: another thread may close
    // the descriptor meanwhile. See sched::file_of().
    vfs::File** pins;
    u32 pin_count, pin_capacity;

    void (*entry)(void*);
    void* arg;
    char name[24];
};

// Per-CPU scheduler state. Everything here is guarded by `lock`, which is
// always taken with interrupts disabled.
struct RunQueue {
    Spinlock lock;
    Thread* head;      // ready threads, FIFO: round-robin
    Thread* tail;
    Thread* sleepers;  // unordered; scanned each tick
    u64 switches;
};

} // namespace sched
