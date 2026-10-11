/* SYSCALL entry and SYSRET exit.
 *
 * SYSCALL leaves the user's RSP untouched, puts the return RIP in rcx and
 * RFLAGS in r11, masks RFLAGS with SFMASK (so interrupts are off here), and
 * does nothing else: no stack switch, no GS swap. Both happen by hand below,
 * using the per-CPU record that SWAPGS makes reachable:
 *   %gs:8   kernel stack top of the running thread
 *   %gs:16  scratch slot for the user RSP
 *
 * The frame built on the kernel stack is struct SyscallFrame in syscall.cpp;
 * the push order here is the reverse of its field order. */

.section .text

.global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:16
    movq %gs:8, %rsp

    /* An iret-shaped tail, so the frame reads like an interrupt's. */
    pushq $0x1b              /* user SS */
    pushq %gs:16             /* user RSP -- off the scratch slot before anything
                                else can run on this CPU and reuse it */
    pushq %r11               /* user RFLAGS */
    pushq $0x23              /* user CS */
    pushq %rcx               /* user RIP */

    pushq %rax
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* 18 quadwords below a page-aligned stack top: rsp is 16-aligned. The
     * system call may block or be preempted, so interrupts come back on. */
    sti
    movq %rsp, %rdi
    call syscall_dispatch
    cli

.Lsyscall_exit:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax

    popq %rcx                /* user RIP, for SYSRET */
    addq $8, %rsp            /* CS */
    popq %r11                /* user RFLAGS, for SYSRET */
    popq %rsp                /* user RSP; SS is fixed by SYSRET */

    /* Interrupts stay off until SYSRET reloads RFLAGS, so nothing can arrive
     * between here and ring 3 while GS already holds the user's base. */
    swapgs
    sysretq

/* void syscall_return_from(const SyscallFrame* frame)
 *
 * Start ring 3 from a saved frame: copy it to the top of the current thread's
 * kernel stack (where syscall_entry would have built it) and leave through
 * the normal exit path. Nothing below the copy is needed again, so
 * overwriting this thread's own call frames is harmless -- and the frame may
 * itself be in one of them: it is then below where it is going, so it is
 * copied from its end backwards, which never overwrites what is still to be
 * read. */
.global syscall_return_from
syscall_return_from:
    cli
    leaq 136(%rdi), %rsi         /* source: the frame's last word */
    movq %gs:8, %rdi
    leaq -144(%rdi), %rdx        /* destination: sizeof(SyscallFrame) below the top */
    subq $8, %rdi
    movl $18, %ecx
    std
    rep movsq
    cld
    movq %rdx, %rsp
    jmp .Lsyscall_exit
