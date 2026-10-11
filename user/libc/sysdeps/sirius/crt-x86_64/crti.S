/* Prologues of _init and _fini. The linker places crtbegin.o's and
 * crtend.o's .init/.fini fragments between these and crtn.o's epilogues. */

.section .init
.global _init
.type _init, @function
_init:
	pushq %rbp
	movq %rsp, %rbp

.section .fini
.global _fini
.type _fini, @function
_fini:
	pushq %rbp
	movq %rsp, %rbp

.section .note.GNU-stack,"",%progbits
