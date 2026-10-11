/* What the system monitor and `ps` ask the kernel: the state of the machine,
 * and the list of processes. Plain C. */
#ifndef SIRIUS_SYSINFO_H
#define SIRIUS_SYSINFO_H

#include <stdint.h>

#define SIRIUS_MAX_CPUS 64

/* sysinfo(struct sirius_sysinfo*) */
struct sirius_sysinfo {
    uint64_t uptime_ns;
    uint64_t memory_total;            /* bytes of usable RAM */
    uint64_t memory_free;
    uint32_t cpu_count;
    uint32_t process_count;
    uint32_t tick_hz;                 /* what one tick below is worth */
    uint32_t reserved;
    /* Per CPU, since boot: timer ticks taken, and how many of them found the
     * CPU idle. Load over an interval is the difference of two readings. */
    uint64_t cpu_ticks[SIRIUS_MAX_CPUS];
    uint64_t cpu_idle_ticks[SIRIUS_MAX_CPUS];
};

/* proclist(struct sirius_procinfo*, capacity) -> number of processes written */
struct sirius_procinfo {
    uint32_t pid;
    uint32_t parent;                  /* 0 if it has none */
    uint32_t cpu;                     /* the CPU it runs on */
    uint32_t state;                   /* SIRIUS_PROC_* */
    uint64_t cpu_ticks;               /* timer ticks that found it running */
    char     name[32];
};

#define SIRIUS_PROC_RUNNING   0       /* running or ready to */
#define SIRIUS_PROC_SLEEPING  1       /* waiting for something */
#define SIRIUS_PROC_ZOMBIE    2       /* exited, not yet waited for */

#endif
