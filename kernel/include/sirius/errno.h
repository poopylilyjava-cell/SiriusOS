/* Error numbers returned (negated) by Sirius system calls.
 *
 * The values are Linux's, so that mlibc's existing x86-64 errno table is
 * used unchanged. */
#ifndef SIRIUS_ERRNO_H
#define SIRIUS_ERRNO_H

#define SIRIUS_EPERM        1
#define SIRIUS_ENOENT       2
#define SIRIUS_ESRCH        3
#define SIRIUS_EINTR        4
#define SIRIUS_EIO          5
#define SIRIUS_E2BIG        7
#define SIRIUS_ENOEXEC      8
#define SIRIUS_EBADF        9
#define SIRIUS_ECHILD      10
#define SIRIUS_EAGAIN      11
#define SIRIUS_ENOMEM      12
#define SIRIUS_EACCES      13
#define SIRIUS_EFAULT      14
#define SIRIUS_EBUSY       16
#define SIRIUS_EEXIST      17
#define SIRIUS_EXDEV       18
#define SIRIUS_ENODEV      19
#define SIRIUS_ENOTDIR     20
#define SIRIUS_EISDIR      21
#define SIRIUS_EINVAL      22
#define SIRIUS_EMFILE      24
#define SIRIUS_ENOTTY      25
#define SIRIUS_EFBIG       27
#define SIRIUS_ENOSPC      28
#define SIRIUS_ESPIPE      29
#define SIRIUS_EROFS       30
#define SIRIUS_EPIPE       32
#define SIRIUS_ENAMETOOLONG 36
#define SIRIUS_ENOSYS      38
#define SIRIUS_ENOTEMPTY   39
#define SIRIUS_ENOTSOCK    88
#define SIRIUS_EOPNOTSUPP  95
#define SIRIUS_EAFNOSUPPORT 97
#define SIRIUS_EADDRINUSE  98
#define SIRIUS_EISCONN    106
#define SIRIUS_ENOTCONN   107
#define SIRIUS_ETIMEDOUT  110
#define SIRIUS_ECONNREFUSED 111

#endif
