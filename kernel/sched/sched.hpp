#pragma once
#include <sched/thread.hpp>

struct Registers;

// Preemptive round-robin scheduling, one run queue per CPU.
//
// Lock order, outermost first: a wait queue's own lock (Mutex), then a run
// queue lock. The heap and page allocator locks are leaves below both.
namespace sched {

// Turn the code already running on this CPU into its idle thread. Its stack
// (from vmm::create_kernel_stack) must have been made by the caller.
void init_bsp(u64 boot_stack_top);
void init_ap(u64 boot_stack_top);

// Create a thread running entry(arg), without starting it. It is placed on an
// online CPU, round robin, unless `cpu` names one. Returns null if out of
// memory. Until start(), the caller may still adjust it (fork copies the
// parent's FPU state in).
Thread* create(const char* name, void (*entry)(void*), void* arg,
               Process* process = nullptr, Cpu* cpu = nullptr);
void start(Thread* thread);

// create() and start() together.
Thread* spawn(const char* name, void (*entry)(void*), void* arg,
              Process* process = nullptr, Cpu* cpu = nullptr);

// Enable interrupts and idle. The scheduler takes over from the next tick.
[[noreturn]] void run_idle();

Thread* current();

void yield();
void sleep_ms(u64 milliseconds);
[[noreturn]] void exit_current();

// Called by the timer interrupt, with interrupts disabled.
void tick();

// Called by the reschedule IPI, with interrupts disabled: another CPU has put
// work on this one's run queue. An idle CPU takes it at once. A busy one
// does if the work is a thread that was woken from a wait; a thread that was
// merely started waits for the next tick, and the running one keeps its slice.
void reschedule_interrupt();

// A point where the running thread can be switched away from: the end of a
// system call, or of an interrupt that arrived while interrupts were on (so
// no spinlock is held). If a thread has been woken on this CPU since the
// last switch, it runs now and the caller goes to the back of the queue.
void preempt_point();

// Block the current thread. The caller holds `held`, taken with
// lock_irqsave() returning `flags`; it is released only once the thread is
// safely off its CPU, so a wake() cannot be lost in between.
void block_current(Spinlock& held, u64 flags);

// Make a Blocked thread runnable. Does nothing to a thread in any other state.
void wake(Thread* thread);

// Milliseconds since the scheduler started, at tick resolution.
u64 uptime_ms();

// Kill the current user thread's process after an exception it caused.
[[noreturn]] void kill_current_for_fault(const Registers* regs);

// An exception in user mode: run the program's signal handler for it if it
// has one (returning, with `regs` entering the handler), or kill it.
void fault_in_user_mode(Registers* regs);

struct Stats { u64 switches; u64 threads_created; u64 threads_reaped; };
Stats stats();

} // namespace sched
