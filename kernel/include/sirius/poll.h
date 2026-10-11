/* Waiting for descriptors: poll, epoll, and the descriptors that exist to be
 * waited on (timerfd, signalfd, eventfd). Layouts and numbers are Linux's on
 * x86-64. Plain C. */
#ifndef SIRIUS_POLL_H
#define SIRIUS_POLL_H

#include <stdint.h>

/* Readiness: the same bits in poll's events and epoll's. */
#define SIRIUS_POLLIN    0x0001
#define SIRIUS_POLLOUT   0x0004
#define SIRIUS_POLLERR   0x0008   /* reported whether asked for or not, as are the next two */
#define SIRIUS_POLLHUP   0x0010
#define SIRIUS_POLLNVAL  0x0020   /* poll only: not an open descriptor */

struct sirius_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

#define SIRIUS_EPOLL_CTL_ADD  1
#define SIRIUS_EPOLL_CTL_DEL  2
#define SIRIUS_EPOLL_CTL_MOD  3

/* Level-triggered only: an event is reported for as long as it is true. */
struct sirius_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

/* timerfd: reading gives a uint64_t count of expirations since the last read. */
#define SIRIUS_TFD_TIMER_ABSTIME  1
struct sirius_timespec {
    int64_t sec;
    int64_t nsec;
};
struct sirius_itimerspec {
    struct sirius_timespec interval;   /* zero: one shot */
    struct sirius_timespec value;      /* zero: disarmed */
};

/* signalfd: reading gives one of these per pending signal. 128 bytes. */
struct sirius_signalfd_siginfo {
    uint32_t signo;
    uint8_t  reserved[124];
};

/* eventfd: an 8-byte counter. EFD_SEMAPHORE makes a read take 1, not all. */
#define SIRIUS_EFD_SEMAPHORE  1

/* fcntl() */
#define SIRIUS_F_DUPFD          0
#define SIRIUS_F_GETFD          1
#define SIRIUS_F_SETFD          2
#define SIRIUS_F_GETFL          3
#define SIRIUS_F_SETFL          4
#define SIRIUS_F_DUPFD_CLOEXEC  1030
#define SIRIUS_FD_CLOEXEC       1

#endif
