/* Where the kernel starts a new thread (SYS_THREAD_CREATE). The stack it was
 * given holds, on top, the function the thread is to run and its argument,
 * put there by PrepareStack. With those popped the stack pointer is 16-byte
 * aligned, as it must be before a call. __mlibc_enter_thread does not return. */

.section .text
.global __sirius_thread_entry
.type __sirius_thread_entry, @function
__sirius_thread_entry:
	xorl %ebp, %ebp
	popq %rdi
	popq %rsi
	call __mlibc_enter_thread
	ud2
.size __sirius_thread_entry, . - __sirius_thread_entry

.section .note.GNU-stack,"",%progbits
