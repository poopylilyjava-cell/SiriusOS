# ADR 0006 — Per-CPU run queues, threads pinned to a CPU, SYSCALL entry

**Status:** accepted · 2026-10-10 · **milestone:** M2

## Context
M2 adds threads, preemption, the other CPUs, ring 3 and the system call
boundary. These shape every later subsystem that blocks, wakes, or crosses
into the kernel.

## Decisions

### One run queue per CPU, and threads never migrate
Each CPU schedules round-robin from its own queue under its own spinlock. A
new thread is placed on an online CPU round-robin and stays there.

*Rejected for now:* a single global queue (one lock contended by every tick on
every CPU) and work stealing. Migration is the right end state, but it brings
TLB shootdowns with it: a thread that moves leaves stale translations behind
on the CPU it left. Pinning postpones that until IPIs exist.

### The timer is the local APIC, calibrated against the PIT
100 Hz periodic, one tick per quantum. The PIT is used only as a stopwatch at
boot (channel 2, polled). The legacy PICs are remapped above the exceptions and
masked, so a spurious IRQ 7 or 15 cannot masquerade as a CPU fault.

*Known gap:* some recent laptops ship without a PIT. M9 replaces calibration
with the HPET or CPUID's TSC frequency.

### Per-CPU state through GS, with SWAPGS on every ring 3 boundary
`struct Cpu` sits at the GS base in kernel mode; `%gs:0` is a self pointer.
The interrupt path swaps GS only when the saved CS has RPL 3; SYSCALL always
swaps. Each CPU has its own GDT and TSS (the TSS holds that CPU's ring-0 stack
and IST stacks, and a loaded TSS descriptor is marked busy, so it cannot be
shared).

### Blocking holds the run queue lock until the thread is off the CPU
`block_current()` takes the run queue lock *before* releasing the wait-queue
lock, and the lock is only dropped by the next thread, after the switch.
`wake()` takes the same run queue lock, so it cannot run in the gap and lose
the wakeup. Lock order is fixed: wait-queue lock, then run queue lock; the heap
and page allocator locks are leaves. Every spinlock is taken with interrupts
off.

### Dead threads are reaped by their successor
A thread cannot free the stack it is running on. `exit` marks it dead and
switches away; the next thread on that CPU frees it as the first thing it does.
A user process's address space goes the same way, after CR3 has moved on.

### Kernel stack ranges are never reused
Freeing a kernel stack frees its pages but not its virtual range. A thread is
built on one CPU and may run on another, so both can hold TLB entries for its
stack; reusing the range without a shootdown would let a stale entry reach a
reallocated page. The stack region is 512 GiB, so this is good for millions of
threads. Shootdowns lift it later.

### SYSCALL/SYSRET, with the frame saved before interrupts come back
The entry stub switches to the thread's kernel stack, pushes an iret-shaped
frame and all argument registers, and only then re-enables interrupts, so a
system call can block or be preempted like any other kernel code. The ABI
(`kernel/include/sirius/syscall.h`) uses Linux-style registers so mlibc's
x86-64 conventions carry over, but the numbers are provisional (ADR 0002).

### FPU state is saved eagerly, for user threads only
The kernel is built without SSE, so only user threads have FPU state. It is
saved and restored with FXSAVE on every switch between threads that have it.
Lazy switching (#NM traps) saves little on modern CPUs and has leaked state
across processes in other kernels.

### User programs before mlibc are boot modules
`user/bringup/` builds static ELF programs with the kernel's own compiler and
a header-only runtime. Limine loads them as modules, and each module's string
becomes the program's argument. Validation lives in `kernel/exec/elf.cpp`, which
is host-tested against malformed images.

## Consequences
- ~~A thread woken by another CPU waits for its CPU's next tick (≤10 ms) if
  that CPU is idle.~~ Fixed in M3 with a reschedule IPI; see ADR 0007.
- A panic on one CPU does not stop the others yet.
- An AddressSpace does no locking of its own: processes are single-threaded,
  and the shared kernel half changes only under vmm's kernel mapping lock.
  Multi-threaded processes (M3+) need a per-space lock and TLB shootdowns.
- `-cpu` features like x2APIC are handled when present but untested in CI;
  QEMU's qemu64 model runs in xAPIC mode.
