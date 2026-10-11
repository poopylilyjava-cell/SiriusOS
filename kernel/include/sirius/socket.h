/* Sockets: AF_UNIX stream sockets, with file descriptor passing. Layouts and
 * numbers are Linux's on x86-64. Plain C. */
#ifndef SIRIUS_SOCKET_H
#define SIRIUS_SOCKET_H

#include <stdint.h>

#define SIRIUS_AF_UNIX        1
#define SIRIUS_SOCK_STREAM    1
#define SIRIUS_SOCK_NONBLOCK  04000       /* or-ed into the type, as are these two */
#define SIRIUS_SOCK_CLOEXEC   02000000

struct sirius_sockaddr_un {
    uint16_t sun_family;                  /* SIRIUS_AF_UNIX */
    char     sun_path[108];
};

struct sirius_iovec {
    uint64_t base;
    uint64_t length;
};

struct sirius_msghdr {
    uint64_t name;            /* ignored: stream sockets are connected */
    uint32_t name_length;
    uint32_t pad0;
    uint64_t iov;             /* struct sirius_iovec[] */
    uint64_t iov_count;
    uint64_t control;         /* ancillary data: struct sirius_cmsghdr records */
    uint64_t control_length;
    int32_t  flags;           /* recvmsg sets SIRIUS_MSG_CTRUNC here */
    uint32_t pad1;
};

/* One record of ancillary data; its payload follows, and the next record
 * starts at the next multiple of 8 bytes. */
struct sirius_cmsghdr {
    uint32_t length;          /* header and payload */
    uint32_t pad;             /* never initialised by programs: do not read it */
    int32_t  level;           /* SIRIUS_SOL_SOCKET */
    int32_t  type;            /* SIRIUS_SCM_RIGHTS: the payload is an array of int descriptors */
};

#define SIRIUS_SOL_SOCKET   1
#define SIRIUS_SCM_RIGHTS   1
#define SIRIUS_SCM_MAX_FD   253           /* descriptors in one message */

#define SIRIUS_SO_ERROR     4
#define SIRIUS_SO_PEERCRED  17            /* struct sirius_ucred */

struct sirius_ucred {
    int32_t  pid;
    uint32_t uid;
    uint32_t gid;
};

/* sendmsg() and recvmsg() flags */
#define SIRIUS_MSG_CTRUNC        0x0008   /* out: ancillary data did not fit */
#define SIRIUS_MSG_DONTWAIT      0x0040
#define SIRIUS_MSG_NOSIGNAL      0x4000   /* EPIPE without SIGPIPE */
#define SIRIUS_MSG_CMSG_CLOEXEC  0x40000000

/* shutdown() */
#define SIRIUS_SHUT_RD    0
#define SIRIUS_SHUT_WR    1
#define SIRIUS_SHUT_RDWR  2

#endif
