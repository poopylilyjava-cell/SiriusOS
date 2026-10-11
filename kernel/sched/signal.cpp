#include <sched/signal.hpp>
#include <sched/events.hpp>
#include <sched/process.hpp>
#include <sched/sched.hpp>
#include <arch/x86_64/clock.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/syscall.hpp>
#include <mm/heap.hpp>
#include <mm/usercopy.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

extern "C" [[noreturn]] void user_return_from_registers(const Registers* regs);

namespace signal {
namespace {

constexpr u64 bit(int sig) { return 1ULL << (sig - 1); }

// SIGKILL and SIGSTOP cannot be caught, blocked or ignored.
constexpr u64 kUnblockable = bit(SIRIUS_SIGKILL) | bit(SIRIUS_SIGSTOP);

// Signals whose default action is to do nothing. Stopping and continuing
// are not implemented, so the job-control signals are among them.
constexpr u64 kDefaultIgnored = bit(SIRIUS_SIGCHLD) | bit(SIRIUS_SIGURG) | bit(SIRIUS_SIGWINCH) | bit(SIRIUS_SIGCONT) |
                                bit(SIRIUS_SIGSTOP) | bit(SIRIUS_SIGTSTP) | bit(SIRIUS_SIGTTIN) | bit(SIRIUS_SIGTTOU);

// RFLAGS bits a program may set for itself: the arithmetic flags, the
// direction flag and the trap flag. Everything else is the kernel's.
constexpr u64 kUserFlags = 0xdd5;
constexpr u64 kFixedFlags = 0x202;   // interrupts on, and the always-one bit

// Every register of the interrupted program.
struct UserContext {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 rip, rflags, rsp;
};

// What is pushed on the user stack for a handler. The handler returns to
// `restorer`, which calls sigreturn, which finds this again just below the
// stack pointer. It is the kernel's own format: handlers get a pointer to
// `context` as their third argument, but it is not a POSIX ucontext_t.
struct SignalFrame {
    u64 restorer;
    u64 reserved;
    sirius_siginfo info;
    struct Context {
        UserContext regs;
        u64 blocked;        // the signal mask to restore
        u64 reserved;
        u8  fpu[cpu::kFpuStateSize];
    } context;
};

bool ignored(const sched::Process* process, int sig) {
    const u64 handler = process->sig_actions[sig].handler;
    return handler == SIRIUS_SIG_IGN || (handler == SIRIUS_SIG_DFL && (kDefaultIgnored & bit(sig)));
}

u64 load(const u64& word) { return __atomic_load_n(&word, __ATOMIC_ACQUIRE); }

UserContext from(const SyscallFrame& f) {
    // SYSRET hands back rcx and r11 holding the return address and flags.
    return { f.r15, f.r14, f.r13, f.r12, f.rflags, f.r10, f.r9, f.r8, f.rbp, f.rdi, f.rsi, f.rdx, f.rip, f.rbx, f.rax,
             f.rip, f.rflags, f.rsp };
}

UserContext from(const Registers& r) {
    return { r.r15, r.r14, r.r13, r.r12, r.r11, r.r10, r.r9, r.r8, r.rbp, r.rdi, r.rsi, r.rdx, r.rcx, r.rbx, r.rax,
             r.rip, r.rflags, r.rsp };
}

Registers to_registers(const UserContext& c) {
    Registers r{};
    r.r15 = c.r15; r.r14 = c.r14; r.r13 = c.r13; r.r12 = c.r12; r.r11 = c.r11; r.r10 = c.r10; r.r9 = c.r9; r.r8 = c.r8;
    r.rbp = c.rbp; r.rdi = c.rdi; r.rsi = c.rsi; r.rdx = c.rdx; r.rcx = c.rcx; r.rbx = c.rbx; r.rax = c.rax;
    r.rip = c.rip; r.rsp = c.rsp;
    r.rflags = (c.rflags & kUserFlags) | kFixedFlags;
    r.cs = gdt::kUserCodeSelector;
    r.ss = gdt::kUserDataSelector;
    return r;
}

[[noreturn]] void terminate(int sig) {
    // This is the process's own thread, about to return to ring 3: it may
    // sleep while its files are closed, like any system call.
    irq::restore(1 << 9);
    sched::exit_process(sig);
}

// Build the handler's frame on the user stack and point `context` at the
// handler. False if the stack cannot take it.
bool enter_handler(sched::Thread* thread, int sig, const sirius_sigaction& action, UserContext& context, u64 mask_to_restore) {
    sched::Process* process = thread->process;
    auto* frame = static_cast<SignalFrame*>(kzalloc(sizeof(SignalFrame)));
    if (frame == nullptr) return false;
    frame->restorer = action.restorer;
    frame->info.si_signo = sig;
    frame->context.regs = context;
    frame->context.blocked = mask_to_restore;
    // The kernel never touches the FPU, so the registers are still the program's.
    alignas(16) u8 fpu[cpu::kFpuStateSize];
    cpu::fxsave(fpu);
    memcpy(frame->context.fpu, fpu, sizeof(fpu));

    // Below the red zone the ABI lets the program use without moving rsp,
    // and aligned as at the entry of any function: 8 below a multiple of 16,
    // as if a call had just pushed the return address.
    const u64 address = ((context.rsp - 128 - sizeof(SignalFrame)) & ~15ULL) - 8;
    const int status = usercopy::copy_to(address, frame, sizeof(SignalFrame));
    kfree(frame);
    if (status < 0) return false;

    context.rsp = address;
    context.rip = action.handler;
    context.rdi = static_cast<u64>(sig);
    context.rsi = address + __builtin_offsetof(SignalFrame, info);
    context.rdx = address + __builtin_offsetof(SignalFrame, context);
    context.rflags &= ~(1ULL << 10);   // the ABI wants the direction flag clear on entry

    u64 blocked = thread->sig_blocked | action.mask;
    if (!(action.flags & SIRIUS_SA_NODEFER)) blocked |= bit(sig);
    __atomic_store_n(&thread->sig_blocked, blocked & ~kUnblockable, __ATOMIC_RELEASE);
    if (action.flags & SIRIUS_SA_RESETHAND) process->sig_actions[sig].handler = SIRIUS_SIG_DFL;
    return true;
}

// Act on the pending signals. Returns true if `context` now enters a handler.
// `syscall_result` is null when the process was not in a system call.
bool deliver(sched::Thread* thread, UserContext& context, bool in_syscall, u64 syscall_number) {
    sched::Process* process = thread->process;
    // What a handler's return must restore: normally the mask as it is now,
    // but after sigsuspend the one from before the call.
    u64 mask_to_restore = thread->sig_blocked;
    if (thread->sig_has_saved_mask) mask_to_restore = thread->sig_saved_mask;

    bool entered = false;
    for (;;) {
        const u64 ready = (load(thread->sig_pending) | load(process->sig_pending)) & ~thread->sig_blocked;
        if (ready == 0) break;
        const int sig = __builtin_ctzll(ready) + 1;
        // Its own first; a signal for the process belongs to whichever
        // thread clears the bit.
        if (!(__atomic_fetch_and(&thread->sig_pending, ~bit(sig), __ATOMIC_ACQ_REL) & bit(sig)) &&
            !(__atomic_fetch_and(&process->sig_pending, ~bit(sig), __ATOMIC_ACQ_REL) & bit(sig))) continue;

        if (ignored(process, sig)) continue;
        const sirius_sigaction action = process->sig_actions[sig];
        if (action.handler == SIRIUS_SIG_DFL) terminate(sig);

        // The interrupted system call either starts again when the handler
        // returns, or is seen to fail with EINTR.
        if (in_syscall && static_cast<i64>(context.rax) == kRestart) {
            if (action.flags & SIRIUS_SA_RESTART) { context.rax = syscall_number; context.rip -= 2; }   // back onto the SYSCALL instruction
            else context.rax = static_cast<u64>(-SIRIUS_EINTR);
        }
        if (!enter_handler(thread, sig, action, context, mask_to_restore)) terminate(SIRIUS_SIGSEGV);
        entered = true;
        break;
    }

    if (thread->sig_has_saved_mask) {
        thread->sig_has_saved_mask = false;
        // With a handler on the way its return restores the mask; otherwise now.
        if (!entered) __atomic_store_n(&thread->sig_blocked, thread->sig_saved_mask, __ATOMIC_RELEASE);
    }
    // Woken by a signal that turned out to need no handler: carry on with
    // the call as if nothing had happened.
    if (!entered && in_syscall && static_cast<i64>(context.rax) == kRestart) {
        context.rax = syscall_number;
        context.rip -= 2;
    }
    return entered;
}

u64 timeval_ns(i64 seconds, i64 microseconds) {
    if (seconds < 0 || microseconds < 0) return 0;
    return static_cast<u64>(seconds) * 1'000'000'000 + static_cast<u64>(microseconds) * 1000;
}

} // namespace

// An ignored signal is not discarded here but by the thread that takes it:
// whether it is blocked -- which is how a signalfd collects signals the
// process otherwise leaves alone -- depends on the thread.
void post(sched::Process* process, int sig) {
    if (sig < 1 || sig >= SIRIUS_NSIG) return;
    __atomic_or_fetch(&process->sig_pending, bit(sig), __ATOMIC_ACQ_REL);
    events::notify();
}

void post(sched::Thread* thread, int sig) {
    if (sig < 1 || sig >= SIRIUS_NSIG) return;
    __atomic_or_fetch(&thread->sig_pending, bit(sig), __ATOMIC_ACQ_REL);
    events::notify();
}

bool deliverable(const sched::Thread* thread) {
    if (sched::dying(thread)) return true;
    const sched::Process* process = thread->process;
    u64 ready = (load(thread->sig_pending) | load(process->sig_pending)) & ~load(thread->sig_blocked);
    while (ready) {
        const int sig = __builtin_ctzll(ready) + 1;
        if (!ignored(process, sig)) return true;
        ready &= ~bit(sig);
    }
    return false;
}

// Does `thread` have anything to do before it goes back to user mode?
static bool nothing_due(const sched::Thread* thread) {
    return !thread->sig_has_saved_mask &&
           ((load(thread->sig_pending) | load(thread->process->sig_pending)) & ~thread->sig_blocked) == 0;
}

void on_syscall_return(SyscallFrame& frame, u64 number) {
    sched::Thread* thread = sched::current();
    if (thread->process == nullptr) return;
    if (sched::dying(thread)) sched::exit_thread();
    const bool restart = static_cast<i64>(frame.rax) == kRestart;
    if (!restart && nothing_due(thread)) return;

    UserContext context = from(frame);
    deliver(thread, context, true, number);
    // A system call's frame has no rcx or r11 of its own (SYSRET overwrites
    // them); everything else is put back, changed or not.
    frame.r15 = context.r15; frame.r14 = context.r14; frame.r13 = context.r13; frame.r12 = context.r12;
    frame.r10 = context.r10; frame.r9 = context.r9; frame.r8 = context.r8;
    frame.rbp = context.rbp; frame.rdi = context.rdi; frame.rsi = context.rsi; frame.rdx = context.rdx;
    frame.rbx = context.rbx; frame.rax = context.rax;
    frame.rip = context.rip; frame.rsp = context.rsp;
    frame.rflags = (context.rflags & kUserFlags) | kFixedFlags;
}

void on_interrupt_return(Registers* regs) {
    sched::Thread* thread = sched::current();
    if (thread->process == nullptr) return;
    if (sched::dying(thread)) sched::exit_thread();
    if (nothing_due(thread)) return;

    UserContext context = from(*regs);
    if (!deliver(thread, context, false, 0)) return;
    const Registers entered = to_registers(context);
    const u64 vector = regs->vector, error_code = regs->error_code;
    *regs = entered;
    regs->vector = vector;
    regs->error_code = error_code;
}

void raise_fault(Registers* regs, int sig) {
    sched::Thread* thread = sched::current();
    // A fault while the signal is blocked or ignored would just fault again
    // for ever: the only thing left is the default action.
    if (thread->process->sig_actions[sig].handler <= SIRIUS_SIG_IGN || (thread->sig_blocked & bit(sig))) terminate(sig);
    // It is this thread's fault, and this thread's signal.
    __atomic_or_fetch(&thread->sig_pending, bit(sig), __ATOMIC_ACQ_REL);
    on_interrupt_return(regs);
}

void sys_sigreturn(const SyscallFrame& frame) {
    sched::Thread* thread = sched::current();
    // The handler's `ret` popped the restorer's address, so the frame starts
    // one word below the stack pointer the restorer made the call with.
    auto* saved = static_cast<SignalFrame*>(kmalloc(sizeof(SignalFrame)));
    if (saved == nullptr || usercopy::copy_from(saved, frame.rsp - 8, sizeof(SignalFrame)) < 0) {
        kfree(saved);
        terminate(SIRIUS_SIGSEGV);
    }

    __atomic_store_n(&thread->sig_blocked, saved->context.blocked & ~kUnblockable, __ATOMIC_RELEASE);

    // The FPU image comes from user memory: loading reserved MXCSR bits
    // would fault, here, in the kernel.
    alignas(16) u8 fpu[cpu::kFpuStateSize];
    memcpy(fpu, saved->context.fpu, sizeof(fpu));
    u32 mxcsr;
    memcpy(&mxcsr, fpu + 24, sizeof(mxcsr));
    mxcsr &= 0xffbf;
    memcpy(fpu + 24, &mxcsr, sizeof(mxcsr));
    cpu::fxrstor(fpu);

    Registers regs = to_registers(saved->context.regs);
    kfree(saved);
    // Another signal may have become deliverable now that the mask is back.
    if (regs.rip >= 0x0000800000000000ULL) terminate(SIRIUS_SIGSEGV);   // not an address ring 3 could return to
    on_interrupt_return(&regs);
    user_return_from_registers(&regs);
}

i64 sys_kill(i64 pid, u64 sig) {
    if (sig >= SIRIUS_NSIG) return -SIRIUS_EINVAL;
    // 0 is the caller's group, -n group n. -1 would be every process the
    // caller may signal; with no permissions that is everyone, which is
    // never what is meant here: it is the caller alone.
    if (pid == 0) return sched::signal_group(sched::current_process()->pgid, static_cast<int>(sig));
    if (pid == -1) {
        if (sig) post(sched::current_process(), static_cast<int>(sig));
        return 0;
    }
    if (pid < 0) return sched::signal_group(static_cast<u32>(-pid), static_cast<int>(sig));
    return sched::signal_process(static_cast<u32>(pid), static_cast<int>(sig));
}

i64 sys_tkill(u64 tid, u64 sig) {
    if (sig >= SIRIUS_NSIG || tid > 0xffffffffULL) return -SIRIUS_EINVAL;
    return sched::signal_thread(static_cast<u32>(tid), static_cast<int>(sig));
}

i64 sys_sigaction(u64 sig, u64 action_address, u64 old_address) {
    if (sig < 1 || sig >= SIRIUS_NSIG) return -SIRIUS_EINVAL;
    sched::Process* process = sched::current_process();

    sirius_sigaction action{};
    if (action_address) {
        if (bit(static_cast<int>(sig)) & kUnblockable) return -SIRIUS_EINVAL;
        if (const int status = usercopy::copy_from(&action, action_address, sizeof(action)); status < 0) return status;
        if (action.handler > SIRIUS_SIG_IGN && !(action.flags & SIRIUS_SA_RESTORER)) return -SIRIUS_EINVAL;
    }
    if (old_address) {
        if (const int status = usercopy::copy_to(old_address, &process->sig_actions[sig], sizeof(action)); status < 0) return status;
    }
    if (action_address) {
        process->sig_actions[sig] = action;
        // Ignoring a signal discards any instance already pending.
        if (ignored(process, static_cast<int>(sig)))
            __atomic_and_fetch(&process->sig_pending, ~bit(static_cast<int>(sig)), __ATOMIC_ACQ_REL);
    }
    return 0;
}

// The calling thread's mask: in a process with one thread, the process's.
i64 sys_sigprocmask(u64 how, u64 set_address, u64 old_address) {
    sched::Thread* thread = sched::current();
    const u64 old = thread->sig_blocked;
    if (set_address) {
        u64 set;
        if (const int status = usercopy::copy_from(&set, set_address, sizeof(set)); status < 0) return status;
        u64 blocked;
        switch (how) {
        case SIRIUS_SIG_BLOCK:   blocked = old | set; break;
        case SIRIUS_SIG_UNBLOCK: blocked = old & ~set; break;
        case SIRIUS_SIG_SETMASK: blocked = set; break;
        default: return -SIRIUS_EINVAL;
        }
        __atomic_store_n(&thread->sig_blocked, blocked & ~kUnblockable, __ATOMIC_RELEASE);
    }
    if (old_address) return usercopy::copy_to(old_address, &old, sizeof(old));
    return 0;
}

i64 sys_sigpending(u64 set_address) {
    const sched::Thread* thread = sched::current();
    const u64 pending = (load(thread->sig_pending) | load(thread->process->sig_pending)) & thread->sig_blocked;
    return usercopy::copy_to(set_address, &pending, sizeof(pending));
}

void set_temporary_mask(u64 mask) {
    sched::Thread* thread = sched::current();
    thread->sig_saved_mask = thread->sig_blocked;
    thread->sig_has_saved_mask = true;
    __atomic_store_n(&thread->sig_blocked, mask & ~kUnblockable, __ATOMIC_RELEASE);
}

void drop_temporary_mask() {
    sched::Thread* thread = sched::current();
    if (!thread->sig_has_saved_mask) return;
    thread->sig_has_saved_mask = false;
    __atomic_store_n(&thread->sig_blocked, thread->sig_saved_mask, __ATOMIC_RELEASE);
}

i64 sys_sigsuspend(u64 mask_address) {
    u64 mask;
    if (const int status = usercopy::copy_from(&mask, mask_address, sizeof(mask)); status < 0) return status;

    set_temporary_mask(mask);
    while (!deliverable(sched::current())) events::wait(events::snapshot());
    // Delivery, on the way out of this call, runs the handler under the
    // temporary mask and then puts the old one back.
    return -SIRIUS_EINTR;
}

i64 sys_setitimer(u64 which, u64 new_address, u64 old_address) {
    if (which != SIRIUS_ITIMER_REAL) return -SIRIUS_EINVAL;
    sched::Process* process = sched::current_process();

    sirius_itimerval value{};
    if (new_address) {
        if (const int status = usercopy::copy_from(&value, new_address, sizeof(value)); status < 0) return status;
    }
    const u64 now = clock::now_ns();
    if (old_address) {
        const u64 deadline = load(process->alarm_deadline_ns);
        const u64 left = deadline > now ? deadline - now : 0;
        sirius_itimerval old{};
        old.interval_sec = static_cast<i64>(process->alarm_interval_ns / 1'000'000'000);
        old.interval_usec = static_cast<i64>(process->alarm_interval_ns % 1'000'000'000 / 1000);
        old.value_sec = static_cast<i64>(left / 1'000'000'000);
        // A timer that is armed never reads as zero, which would mean disarmed.
        old.value_usec = static_cast<i64>(left % 1'000'000'000 / 1000) + (deadline && left < 1000 ? 1 : 0);
        if (const int status = usercopy::copy_to(old_address, &old, sizeof(old)); status < 0) return status;
    }
    if (new_address) {
        const u64 delay = timeval_ns(value.value_sec, value.value_usec);
        process->alarm_interval_ns = timeval_ns(value.interval_sec, value.interval_usec);
        __atomic_store_n(&process->alarm_deadline_ns, delay ? now + delay : 0, __ATOMIC_RELEASE);
    }
    return 0;
}

void on_fork(sched::Process* parent, sched::Process* child) {
    // The child inherits what the parent does about signals, but none of the
    // signals themselves, and no timer.
    // (The mask is the forking thread's, and goes to the child's thread.)
    memcpy(child->sig_actions, parent->sig_actions, sizeof(child->sig_actions));
}

void on_exec(sched::Process* process) {
    // Handlers were code in the image that is gone. Ignored stays ignored.
    for (int sig = 1; sig < SIRIUS_NSIG; sig++)
        if (process->sig_actions[sig].handler != SIRIUS_SIG_IGN) process->sig_actions[sig] = {};
    sched::current()->sig_has_saved_mask = false;
}

void tick() { sched::expire_alarms(clock::now_ns()); }

} // namespace signal
