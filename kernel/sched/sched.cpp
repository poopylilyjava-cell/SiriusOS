#include <sched/sched.hpp>
#include <sched/process.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/lapic.hpp>
#include <arch/x86_64/percpu.hpp>
#include <mm/heap.hpp>
#include <mm/vmm.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>

extern "C" void switch_context(u64* save_rsp, u64 load_rsp);
extern "C" void thread_start();
extern "C" [[noreturn]] void sched_thread_main(sched::Thread* thread);

namespace sched {
namespace {

constexpr u64 kKernelStackPages = 16;  // 64 KiB

u32 g_next_tid = 1;
u32 g_next_cpu = 0;
u64 g_uptime_ticks = 0;  // counted on the BSP only
Stats g_stats{};

void push_ready(RunQueue& rq, Thread* thread) {
    thread->next = nullptr;
    if (rq.tail) rq.tail->next = thread;
    else         rq.head = thread;
    rq.tail = thread;
}

Thread* pop_ready(RunQueue& rq) {
    Thread* thread = rq.head;
    if (thread == nullptr) return nullptr;
    rq.head = thread->next;
    if (rq.head == nullptr) rq.tail = nullptr;
    thread->next = nullptr;
    return thread;
}

void copy_name(char* dest, usize size, const char* src) {
    usize i = 0;
    for (; src && src[i] && i + 1 < size; i++) dest[i] = src[i];
    dest[i] = '\0';
}

vmm::AddressSpace* space_for(const Thread* thread) {
    return thread->process ? &thread->process->space : &vmm::kernel_space();
}

// Free everything a dead thread owned. Runs on the thread's own CPU, after it
// has switched away for good, so nothing is still executing on its stack.
void reap(Thread* thread) {
    // Only a process's last thread still belongs to it here.
    if (thread->process) release_process(thread->process);
    if (thread->kernel_stack_pages)
        vmm::destroy_kernel_stack(thread->kernel_stack_top, thread->kernel_stack_pages);
    kfree(thread->fpu_state);
    kfree(thread->pins);
    kfree(thread);
    __atomic_fetch_add(&g_stats.threads_reaped, 1, __ATOMIC_RELAXED);
}

// The second half of every context switch, run by the thread being switched
// *to*: drop the run queue lock the outgoing thread took, and bury it if it
// was exiting.
void finish_switch() {
    Cpu* cpu = percpu::current();
    Thread* prev = cpu->switch_prev;
    cpu->switch_prev = nullptr;
    cpu->runqueue->lock.unlock();

    if (prev && prev->state == ThreadState::Dead) reap(prev);
}

// Pick the next thread and switch to it. Called with interrupts off and the
// run queue lock held; the current thread's state has already been set and,
// if it is to run again, it is already queued. Returns, with the lock
// released, when this thread is next scheduled.
void reschedule_locked(Cpu* cpu) {
    RunQueue& rq = *cpu->runqueue;
    Thread* prev = cpu->current;
    Thread* next = pop_ready(rq);
    if (next == nullptr) next = cpu->idle;

    if (next == prev) {
        prev->state = ThreadState::Running;
        rq.lock.unlock();
        return;
    }

    __atomic_store_n(&cpu->need_resched, false, __ATOMIC_RELAXED);
    next->state = ThreadState::Running;
    cpu->current = next;
    cpu->switch_prev = prev;
    rq.switches++;

    // Where the CPU lands on the next interrupt or system call from ring 3.
    gdt::set_kernel_stack(*cpu, next->kernel_stack_top);
    cpu->syscall_stack_top = next->kernel_stack_top;

    vmm::AddressSpace* space = space_for(next);
    if (cpu->address_space != space) space->activate();

    // The kernel never touches the FPU, so only user state needs moving.
    if (prev->fpu_state) cpu::fxsave(prev->fpu_state);
    if (next->fpu_state) cpu::fxrstor(next->fpu_state);
    // FS is the user's thread pointer. Ring 3 cannot change the base itself
    // (CR4.FSGSBASE is off), so the copy in the Thread is always current and
    // only loading is needed. The kernel does not use FS.
    if (next->process) cpu::wrmsr(cpu::msr::kFsBase, next->fs_base);

    switch_context(&prev->saved_rsp, next->saved_rsp);
    finish_switch();
}

// A thread was just queued on `cpu`. If that is another CPU and it is idle,
// it would otherwise not notice before its next tick -- up to 10 ms.
void kick(Cpu* cpu) {
    if (cpu == percpu::current()) return;
    if (__atomic_load_n(&cpu->current, __ATOMIC_RELAXED) != cpu->idle) return;
    lapic::send_ipi(cpu->lapic_id, static_cast<u8>(kVectorReschedule));
}

// A thread that was waiting for something has it now, on `cpu`. It gets the
// CPU ahead of whatever is running there, which goes to the back of the
// queue: otherwise a thread that sleeps a lot -- one doing I/O -- would get
// one turn per tick next to one that never sleeps, and the threads of a
// process share a CPU. The switch itself happens at the CPU's next safe
// point (preempt_point), not here: the caller may hold locks.
void request_preemption(Cpu* cpu) {
    __atomic_store_n(&cpu->need_resched, true, __ATOMIC_RELEASE);
    if (cpu != percpu::current()) lapic::send_ipi(cpu->lapic_id, static_cast<u8>(kVectorReschedule));
}

Cpu* pick_cpu() {
    const u32 count = percpu::count();
    for (u32 attempt = 0; attempt < count; attempt++) {
        const u32 index = __atomic_fetch_add(&g_next_cpu, 1, __ATOMIC_RELAXED) % count;
        Cpu* cpu = percpu::at(index);
        if (cpu && __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) return cpu;
    }
    return percpu::current();
}

void init_cpu(u64 boot_stack_top) {
    Cpu* cpu = percpu::current();

    auto* rq = static_cast<RunQueue*>(kzalloc(sizeof(RunQueue)));
    auto* idle = static_cast<Thread*>(kzalloc(sizeof(Thread)));
    if (rq == nullptr || idle == nullptr) panic("sched: out of memory for cpu %u", cpu->index);

    // The idle thread is the context already running. Its stack is the one
    // the CPU booted onto, and it is never freed.
    idle->kernel_stack_top = boot_stack_top;
    idle->state = ThreadState::Running;
    idle->cpu = cpu;
    idle->tid = 0;
    ksnprintf(idle->name, sizeof(idle->name), "idle/%u", cpu->index);

    cpu->runqueue = rq;
    cpu->idle = idle;
    cpu->current = idle;
    gdt::set_kernel_stack(*cpu, boot_stack_top);
    cpu->syscall_stack_top = boot_stack_top;
    __atomic_store_n(&cpu->online, true, __ATOMIC_RELEASE);
}

} // namespace

void init_bsp(u64 boot_stack_top) {
    init_cpu(boot_stack_top);
    LOG_INFO("sched: round-robin, %u Hz tick, %lu KiB kernel stacks",
             lapic::kTimerHz, kKernelStackPages * mm::kPageSize / 1024);
}

void init_ap(u64 boot_stack_top) { init_cpu(boot_stack_top); }

Thread* create(const char* name, void (*entry)(void*), void* arg, Process* process, Cpu* cpu) {
    auto* thread = static_cast<Thread*>(kzalloc(sizeof(Thread)));
    if (thread == nullptr) return nullptr;

    const u64 top = vmm::create_kernel_stack(kKernelStackPages);
    if (top == 0) { kfree(thread); return nullptr; }

    if (process != nullptr) {
        thread->fpu_state = kmalloc(cpu::kFpuStateSize);
        if (thread->fpu_state == nullptr) {
            vmm::destroy_kernel_stack(top, kKernelStackPages);
            kfree(thread);
            return nullptr;
        }
        memcpy(thread->fpu_state, cpu::initial_fpu_state(), cpu::kFpuStateSize);
    }

    thread->kernel_stack_top   = top;
    thread->kernel_stack_pages = kKernelStackPages;
    thread->tid     = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
    thread->process = process;
    thread->entry   = entry;
    thread->arg     = arg;
    thread->cpu     = cpu ? cpu : pick_cpu();
    copy_name(thread->name, sizeof(thread->name), name);

    // Build the frame switch_context expects: six callee-saved registers, then
    // a return address. r12 carries the Thread to thread_start.
    auto* stack = reinterpret_cast<u64*>(top);
    *--stack = 0;                                       // alignment padding
    *--stack = reinterpret_cast<u64>(&thread_start);    // ret target
    *--stack = 0;                                       // rbp
    *--stack = 0;                                       // rbx
    *--stack = reinterpret_cast<u64>(thread);           // r12
    *--stack = 0;                                       // r13
    *--stack = 0;                                       // r14
    *--stack = 0;                                       // r15
    thread->saved_rsp = reinterpret_cast<u64>(stack);

    __atomic_fetch_add(&g_stats.threads_created, 1, __ATOMIC_RELAXED);
    LOG_DEBUG("sched: thread %u '%s' on cpu %u", thread->tid, thread->name, thread->cpu->index);
    thread->state = ThreadState::Blocked;   // not runnable until start()
    return thread;
}

void start(Thread* thread) {
    RunQueue& rq = *thread->cpu->runqueue;
    const u64 flags = rq.lock.lock_irqsave();
    thread->state = ThreadState::Ready;
    push_ready(rq, thread);
    rq.lock.unlock_irqrestore(flags);
    kick(thread->cpu);
}

Thread* spawn(const char* name, void (*entry)(void*), void* arg, Process* process, Cpu* cpu) {
    Thread* thread = create(name, entry, arg, process, cpu);
    if (thread) start(thread);
    return thread;
}

[[noreturn]] void run_idle() {
    for (;;) {
        // sti takes effect after the next instruction, so no interrupt can
        // slip in between it and the hlt and leave us sleeping through work.
        __asm__ volatile("sti; hlt" : : : "memory");
    }
}

Thread* current() { return percpu::current()->current; }

void yield() {
    const u64 flags = irq::save();
    Cpu* cpu = percpu::current();
    cpu->runqueue->lock.lock();

    Thread* self = cpu->current;
    if (self != cpu->idle) {
        self->state = ThreadState::Ready;
        push_ready(*cpu->runqueue, self);
    }
    reschedule_locked(cpu);
    irq::restore(flags);
}

void sleep_ms(u64 milliseconds) {
    // Sleeping means at least this long. The tick in progress is already
    // partly spent, so it cannot be counted: one more makes up for it.
    const u64 ticks = (milliseconds * lapic::kTimerHz + 999) / 1000 + 1;

    const u64 flags = irq::save();
    Cpu* cpu = percpu::current();
    RunQueue& rq = *cpu->runqueue;
    rq.lock.lock();

    Thread* self = cpu->current;
    if (self == cpu->idle) panic("sched: the idle thread cannot sleep");
    self->wake_tick = cpu->ticks + ticks;
    self->state = ThreadState::Sleeping;
    self->next = rq.sleepers;
    rq.sleepers = self;

    reschedule_locked(cpu);
    irq::restore(flags);
}

[[noreturn]] void exit_current() {
    irq::save();
    Cpu* cpu = percpu::current();
    cpu->runqueue->lock.lock();

    Thread* self = cpu->current;
    if (self == cpu->idle) panic("sched: the idle thread cannot exit");
    self->state = ThreadState::Dead;
    reschedule_locked(cpu);
    panic("sched: dead thread %u was scheduled again", self->tid);
}

void tick() {
    Cpu* cpu = percpu::current();
    cpu->ticks++;
    if (cpu->index == 0) __atomic_fetch_add(&g_uptime_ticks, 1, __ATOMIC_RELAXED);
    // Accounting by sampling: whoever the tick lands on is charged for it.
    if (cpu->current) {
        if (cpu->current == cpu->idle) cpu->idle_ticks++;
        else __atomic_fetch_add(&cpu->current->run_ticks, 1, __ATOMIC_RELAXED);
    }

    // The scheduler is not up on this CPU yet: early ticks just count.
    if (cpu->runqueue == nullptr) return;

    RunQueue& rq = *cpu->runqueue;
    rq.lock.lock();

    // Wake every sleeper that is due, rebuilding the list without them.
    Thread** link = &rq.sleepers;
    while (Thread* sleeper = *link) {
        if (sleeper->wake_tick <= cpu->ticks) {
            *link = sleeper->next;
            sleeper->state = ThreadState::Ready;
            push_ready(rq, sleeper);
        } else {
            link = &sleeper->next;
        }
    }

    // Nothing else wants this CPU: keep running.
    if (rq.head == nullptr) {
        rq.lock.unlock();
        return;
    }

    Thread* self = cpu->current;
    if (self != cpu->idle) {
        self->state = ThreadState::Ready;
        push_ready(rq, self);
    }
    reschedule_locked(cpu);
}

void block_current(Spinlock& held, u64 flags) {
    Cpu* cpu = percpu::current();
    cpu->runqueue->lock.lock();

    Thread* self = cpu->current;
    if (self == cpu->idle) panic("sched: the idle thread cannot block");
    self->state = ThreadState::Blocked;
    // Safe to let go now: a waker must take our run queue lock to move us,
    // and that is held until we are off the CPU.
    held.unlock();

    reschedule_locked(cpu);
    irq::restore(flags);
}

void wake(Thread* thread) {
    RunQueue& rq = *thread->cpu->runqueue;
    const u64 flags = rq.lock.lock_irqsave();
    bool woken = false;
    if (thread->state == ThreadState::Blocked) {
        thread->state = ThreadState::Ready;
        push_ready(rq, thread);
        woken = true;
    }
    rq.lock.unlock_irqrestore(flags);
    if (woken) request_preemption(thread->cpu);
}

void preempt_point() {
    Cpu* cpu = percpu::current();
    if (cpu->runqueue == nullptr || !__atomic_load_n(&cpu->need_resched, __ATOMIC_ACQUIRE)) return;
    const u64 flags = irq::save();
    RunQueue& rq = *cpu->runqueue;
    rq.lock.lock();
    __atomic_store_n(&cpu->need_resched, false, __ATOMIC_RELAXED);
    if (rq.head == nullptr) {
        rq.lock.unlock();
    } else {
        Thread* self = cpu->current;
        if (self != cpu->idle) {
            self->state = ThreadState::Ready;
            push_ready(rq, self);
        }
        reschedule_locked(cpu);
    }
    irq::restore(flags);
}

void reschedule_interrupt() {
    Cpu* cpu = percpu::current();
    if (cpu->runqueue == nullptr) return;
    RunQueue& rq = *cpu->runqueue;
    rq.lock.lock();
    const bool idle = cpu->current == cpu->idle;
    if (rq.head == nullptr || (!idle && !__atomic_load_n(&cpu->need_resched, __ATOMIC_ACQUIRE))) {
        rq.lock.unlock();
        return;
    }
    // A woken thread takes over from a busy one, which waits its turn again.
    if (!idle) {
        cpu->current->state = ThreadState::Ready;
        push_ready(rq, cpu->current);
    }
    reschedule_locked(cpu);
}

u64 uptime_ms() {
    return __atomic_load_n(&g_uptime_ticks, __ATOMIC_RELAXED) * 1000 / lapic::kTimerHz;
}

Stats stats() {
    Stats result = g_stats;
    result.switches = 0;
    for (u32 i = 0; i < percpu::count(); i++) {
        Cpu* cpu = percpu::at(i);
        if (cpu && cpu->runqueue) result.switches += cpu->runqueue->switches;
    }
    return result;
}

} // namespace sched

extern "C" [[noreturn]] void sched_thread_main(sched::Thread* thread) {
    sched::finish_switch();
    irq::restore(1 << 9);
    thread->entry(thread->arg);
    sched::exit_current();
}
