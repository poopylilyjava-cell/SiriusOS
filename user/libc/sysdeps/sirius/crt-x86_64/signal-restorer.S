/* Where a signal handler returns to. The kernel pushed this address as the
 * handler's return address, above the state it saved; the sigreturn system
 * call (number 34, SYS_SIGRETURN) finds that state from the stack pointer
 * and resumes the interrupted program. It does not come back. */

.section .text
.global __sirius_signal_restorer
.type __sirius_signal_restorer, @function
__sirius_signal_restorer:
	movl $34, %eax
	syscall
	ud2
.size __sirius_signal_restorer, . - __sirius_signal_restorer

.section .note.GNU-stack,"",%progbits
