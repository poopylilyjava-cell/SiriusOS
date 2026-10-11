# ADR 0012 — Threads: several in a process, all on its CPU

**Status:** accepted · 2026-10-11 · after M7; closes what M6 left open

## Context
A process had exactly one thread. `pthread_create` failed, four of
libwayland's 141 tests with it, and anything ported later that assumes threads
would have too. ADR 0006 pins every thread to a CPU and has no TLB shootdowns;
ADR 0010 said threads would need both a per-process lock and shootdowns.

## Decisions

### A process's threads all run on one CPU
A new thread is placed on its creator's CPU. An address space is then only
ever loaded on one CPU, so unmapping a page needs no shootdown, and two
threads of a process are never in the kernel at the same instant. What is
left is that one can be preempted, or go to sleep, in the middle of a system
call while another runs. Each thing they share is guarded against exactly
that:

- **Page tables:** every change is made with interrupts off on that CPU
  (`vmm.cpp`), which makes it atomic for the process's threads and for their
  page faults. `fork` copies the address space in one such step.
- **The descriptor table** has a spinlock. A system call that looks a
  descriptor up (`sched::file_of`) holds a reference to the open file until
  the call returns, so `close` in one thread cannot free a file another is
  blocked on. A process that has never had a second thread skips both.
- **The working directory** a threaded process leaves is kept until it
  exits, since another thread may be resolving a path against it.
- **Memory a system call was using, unmapped by another thread:** the
  kernel checks a user buffer and then uses it, and now the mapping can go
  in between. The resulting fault in kernel mode gives the call a page of
  zeroes to finish with and kills the process (`absorb_kernel_fault`). For a
  process with one thread it is still a kernel bug and a panic.

The cost is plain: threads give concurrency, not parallelism. A process
never uses more than one CPU.

### futex is the only blocking primitive
`futex_wait(word, expected, timeout)` and `futex_wake(word, count)`
(`sched/futex.cpp`); mlibc builds mutexes, condition variables and
`pthread_join` on them. A waiter is listed before the word is read, so a wake
between the look and the sleep is not lost. Waiters sleep on the event
channel (ADR 0010) and are matched by process and address: a futex in memory
shared between two processes does not work across them.

### Signals: the mask is the thread's, the rest is the process's
A signal sent to a process is taken by whichever thread reaches the edge of
user mode first without blocking it. `tkill` sends one to a particular
thread (`pthread_kill`); a fault is the faulting thread's. Actions are
shared. An ignored signal is now discarded by the thread that takes it
rather than when it is posted, because whether it is blocked depends on the
thread.

### One thread ends the others
`exit`, a fatal signal and `execve` all work the same way: the calling thread
becomes the process's *exit leader*; every other thread leaves at its next
return to user mode (one asleep in a system call is woken, and sees `EINTR`);
the leader waits until it is alone, then exits or loads the new program.
`fork` copies the calling thread only. A process whose main thread calls
`pthread_exit` lives until its last thread ends, and exits with status 0.

### A woken thread runs at once
With threads sharing a CPU, "a woken thread waits for the next tick" stopped
being tolerable: next to a thread that never sleeps, one that waits for
something got one turn per tick. `wake()` now asks the thread's CPU to
reschedule, at the end of the current system call or interrupt
(`preempt_point`), or by IPI on another CPU. For the same reason the AHCI
driver, which polls, spins for up to 300 µs before it yields.

## Consequences
- libwayland's suite: 141 of 141. `/bin/threadtest` (39 checks) runs at boot
  with the other tests.
- No parallelism within a process, until threads can migrate -- which needs
  TLB shootdowns and real locks around everything listed above.
- mlibc leaks a joined thread's stack and control block (its own TODO).
  `pthread_cancel` is untested. `std::thread` and `std::mutex` work: GCC was
  already configured for POSIX threads, and now there are some.
- Every futex wake wakes every sleeper in the system, which then look and go
  back to sleep (ADR 0010's trade, now paid more often).
- The limit of 256 descriptors and the process table are unchanged; there is
  no limit on threads but memory.
