/* Process entry. The kernel enters _start with the System V initial stack:
 * rsp points at argc, then argv[], NULL, envp[], NULL and the auxiliary
 * vector, and rsp is 16-byte aligned. __mlibc_entry never returns. */

.section .text
.global _start
.type _start, @function
_start:
	xorl %ebp, %ebp              /* the outermost frame, for debuggers */
	movq %rsp, %rdi              /* the entry stack */
	leaq main(%rip), %rsi
	call __mlibc_entry
	ud2
.size _start, . - _start

.section .note.GNU-stack,"",%progbits
