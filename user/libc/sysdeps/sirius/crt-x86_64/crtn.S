/* Epilogues of _init and _fini; see crti.S. */

.section .init
	popq %rbp
	ret

.section .fini
	popq %rbp
	ret

.section .note.GNU-stack,"",%progbits
