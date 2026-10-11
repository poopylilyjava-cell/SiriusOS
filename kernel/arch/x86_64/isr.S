/* Per-vector interrupt stubs and the shared entry path.
 *
 * Each stub normalises the stack so that isr_common always sees the same
 * layout: [error code][vector] below the CPU-pushed frame. Ten vectors push a
 * real error code in hardware; the rest get a zero placeholder so that the
 * Registers struct in interrupts.hpp describes every vector identically. */

.altmacro

.macro ISR_NO_ERRCODE num
isr_stub_\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERRCODE num
isr_stub_\num:
    /* The CPU already pushed the error code. */
    pushq $\num
    jmp isr_common
.endm

.section .text

.set vec, 0
.rept 256
    .align 16
    /* Vectors that push an error code: #DF, #TS, #NP, #SS, #GP, #PF, #AC,
     * #CP, #VC, #SX. */
    .if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || \
        (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
        ISR_ERRCODE %vec
    .else
        ISR_NO_ERRCODE %vec
    .endif
    .set vec, vec + 1
.endr

isr_common:
    /* The System V ABI requires the direction flag clear on entry to C code,
     * and an interrupt can arrive with it set. */
    cld

    /* Coming from ring 3, GS still holds the user's base; SWAPGS brings in
     * the per-CPU pointer. 24(%rsp) is the saved CS, above the vector and the
     * error code. Its RPL is the privilege level that was interrupted. */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:

    /* Push order is the reverse of the Registers field order. */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    call isr_dispatch

.Lisr_restore:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Going back to ring 3: give the user its GS base back. */
    testb $3, 24(%rsp)
    jz 2f
    swapgs
2:
    /* Discard the vector and error code before returning. */
    addq $16, %rsp
    iretq

/* void user_return_from_registers(const Registers* regs)
 *
 * Enter ring 3 with exactly the state in `regs`, every register included, as
 * if returning from an interrupt that had saved it. This is how a signal
 * handler's return resumes the program it interrupted: SYSRET could not do
 * it, since it overwrites rcx and r11. `regs` may live anywhere, such as on
 * the caller's stack: nothing below it is needed again. */
.global user_return_from_registers
user_return_from_registers:
    cli
    movq %rdi, %rsp
    jmp .Lisr_restore

.macro STUB_ENTRY num
    .quad isr_stub_\num
.endm

.section .rodata
.global isr_stub_table
.align 16
isr_stub_table:
.set vec, 0
.rept 256
    STUB_ENTRY %vec
    .set vec, vec + 1
.endr

/* Stack executability is asserted at link time with -z noexecstack: this
 * bare-metal GCC emits no .note.GNU-stack for any translation unit, so a note
 * here alone would not settle it. */
