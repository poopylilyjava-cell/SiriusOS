/* Process entry for the bring-up programs.
 *
 * The kernel enters _start with the System V initial stack: rsp points at
 * argc, followed by argv[], NULL, envp[], NULL and the auxiliary vector, and
 * rsp is 16-byte aligned. _start is not a C function -- nothing called it --
 * so it builds the C call to main() by hand and passes main's result to exit. */

.section .text
.global _start
_start:
    xorl %ebp, %ebp                   /* the outermost frame, for debuggers */
    movq (%rsp), %rdi                 /* argc */
    leaq 8(%rsp), %rsi                /* argv */
    leaq 16(%rsp, %rdi, 8), %rdx      /* envp: past argv's NULL */
    andq $-16, %rsp
    call main
    movl %eax, %edi
    movl $1, %eax                     /* SYS_EXIT */
    syscall
    ud2
