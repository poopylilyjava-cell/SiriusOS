/* Kernel thread context switch.
 *
 * void switch_context(u64* save_rsp, u64 load_rsp)
 *
 * Saves the callee-saved registers on the current stack, stores the stack
 * pointer through save_rsp, and resumes whatever stack load_rsp describes.
 * Caller-saved registers need no saving: to the compiler this is an ordinary
 * call. The interrupt flag is not part of the switched state -- the scheduler
 * calls this with interrupts off, and each side restores its own flag from
 * its own stack afterwards.
 *
 * A new thread's stack is built by sched::spawn() to look exactly like one
 * that switch_context saved, with thread_start as the return address. */

.section .text

.global switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)

    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* First code a new thread runs. spawn() leaves the Thread pointer in r12. */
.global thread_start
thread_start:
    xorl %ebp, %ebp          /* backtraces stop here */
    andq $-16, %rsp          /* the ABI wants rsp 16-aligned at a call */
    movq %r12, %rdi
    call sched_thread_main
    ud2
