#pragma once
#include <klib/types.hpp>
#include <sirius/signal.h>

struct SyscallFrame;
struct Registers;

// POSIX signals.
//
// A signal is a bit in the target's pending set. It is acted on only by the
// target itself, at the moment it is about to return to user mode -- from a
// system call or from an interrupt -- where its complete user state is at
// hand to be saved. Until then posting one only sets the bit and wakes the
// target if it sleeps (events::notify).
//
// A signal for a process is taken by whichever of its threads gets there
// first without blocking it; each thread has its own mask. What a signal
// does -- the actions -- is the process's.
namespace sched { struct Process; struct Thread; }

namespace signal {

// A result no program sees: the system call was interrupted and, depending
// on the handler's SA_RESTART, is either re-executed or fails with EINTR.
// Calls that must not restart (poll, sleep, sigsuspend) return EINTR itself.
constexpr i64 kRestart = -512;

// Make `sig` pending for `process`, or for one of its threads. Any context.
// A signal that is being ignored is dropped when a thread comes to take it.
void post(sched::Process* process, int sig);
void post(sched::Thread* thread, int sig);

// Is there something `thread` must go back to the edge of user mode for: a
// signal it would act on now -- pending and not blocked -- or the end of its
// process at another thread's hands?
bool deliverable(const sched::Thread* thread);

// Called on the way back to user mode. May not return: the default action of
// most signals ends the process. Otherwise, if a handler is installed, the
// saved user state is rewritten to enter it.
//   after a system call: `number` is the call that was made; frame.rax
//   already holds its result, and a kRestart there is resolved either way.
void on_syscall_return(SyscallFrame& frame, u64 number);
//   after an interrupt or exception taken in user mode.
void on_interrupt_return(Registers* regs);

// A fault in user mode: raise `sig` so that it is acted on at once, even if
// blocked -- there is nothing else the program could execute. Returns only
// if a handler will run.
void raise_fault(Registers* regs, int sig);

// --- system calls ---
i64 sys_kill(i64 pid, u64 sig);
i64 sys_tkill(u64 tid, u64 sig);
i64 sys_sigaction(u64 sig, u64 action_address, u64 old_address);
i64 sys_sigprocmask(u64 how, u64 set_address, u64 old_address);
i64 sys_sigpending(u64 set_address);
i64 sys_sigsuspend(u64 mask_address);
i64 sys_setitimer(u64 which, u64 new_address, u64 old_address);
[[noreturn]] void sys_sigreturn(const SyscallFrame& frame);

// Process lifecycle.
void on_fork(sched::Process* parent, sched::Process* child);
void on_exec(sched::Process* process);

// Replace the caller's blocked set until its next signal delivery has been
// decided (sigsuspend, epoll_pwait): the old set comes back afterwards, and
// is what a handler's return restores.
void set_temporary_mask(u64 mask);
// Undo it if no signal turned up.
void drop_temporary_mask();

// From the timer interrupt on the bootstrap CPU: expire interval timers.
void tick();

} // namespace signal
