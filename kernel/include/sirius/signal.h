/* Signals. Numbers, flags and structure layouts are Linux's on x86-64, as
 * everywhere in this ABI, so mlibc's Linux headers describe them to programs.
 * Plain C. */
#ifndef SIRIUS_SIGNAL_H
#define SIRIUS_SIGNAL_H

#include <stdint.h>

#define SIRIUS_NSIG      65   /* signals are 1..64 */

#define SIRIUS_SIGHUP     1
#define SIRIUS_SIGINT     2
#define SIRIUS_SIGQUIT    3
#define SIRIUS_SIGILL     4
#define SIRIUS_SIGTRAP    5
#define SIRIUS_SIGABRT    6
#define SIRIUS_SIGBUS     7
#define SIRIUS_SIGFPE     8
#define SIRIUS_SIGKILL    9
#define SIRIUS_SIGUSR1   10
#define SIRIUS_SIGSEGV   11
#define SIRIUS_SIGUSR2   12
#define SIRIUS_SIGPIPE   13
#define SIRIUS_SIGALRM   14
#define SIRIUS_SIGTERM   15
#define SIRIUS_SIGCHLD   17
#define SIRIUS_SIGCONT   18
#define SIRIUS_SIGSTOP   19
#define SIRIUS_SIGTSTP   20
#define SIRIUS_SIGTTIN   21
#define SIRIUS_SIGTTOU   22
#define SIRIUS_SIGURG    23
#define SIRIUS_SIGWINCH  28

#define SIRIUS_SIG_DFL   0
#define SIRIUS_SIG_IGN   1

/* sigprocmask() */
#define SIRIUS_SIG_BLOCK    0
#define SIRIUS_SIG_UNBLOCK  1
#define SIRIUS_SIG_SETMASK  2

/* sa_flags */
#define SIRIUS_SA_SIGINFO    0x00000004u
#define SIRIUS_SA_RESTORER   0x04000000u
#define SIRIUS_SA_RESTART    0x10000000u
#define SIRIUS_SA_NODEFER    0x40000000u
#define SIRIUS_SA_RESETHAND  0x80000000u

/* What sigaction() exchanges with the kernel. A handler returns into
 * `restorer`, which must make the sigreturn system call: the C library
 * supplies it, and SA_RESTORER is required. Only the first 64 bits of the
 * library's 1024-bit sigset_t are read or written. */
struct sirius_sigaction {
    uint64_t handler;     /* SIRIUS_SIG_DFL, SIRIUS_SIG_IGN, or void (*)(int, siginfo*, void*) */
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;        /* blocked while the handler runs, with the signal itself */
};

/* The siginfo_t a handler's second argument points at: 128 bytes, of which
 * the kernel fills in these. */
struct sirius_siginfo {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  reserved;
    uint64_t fields[14];
};

/* setitimer(): only the real-time timer, which raises SIGALRM. */
#define SIRIUS_ITIMER_REAL  0
struct sirius_itimerval {
    int64_t interval_sec, interval_usec;   /* reload value; zero for a one-shot */
    int64_t value_sec, value_usec;         /* time until the next expiry; zero disarms */
};

#endif
