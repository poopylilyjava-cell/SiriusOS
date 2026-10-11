/* File system ABI: open flags, file types, and the structures that stat,
 * readdir and mmap exchange with user space. Flag and mode values are Linux's,
 * for the same reason as errno.h. Plain C. */
#ifndef SIRIUS_FS_H
#define SIRIUS_FS_H

#include <stdint.h>

/* open() flags */
#define SIRIUS_O_RDONLY     0x0000
#define SIRIUS_O_WRONLY     0x0001
#define SIRIUS_O_RDWR       0x0002
#define SIRIUS_O_ACCMODE    0x0003
#define SIRIUS_O_CREAT      0x0040
#define SIRIUS_O_EXCL       0x0080
#define SIRIUS_O_TRUNC      0x0200
#define SIRIUS_O_APPEND     0x0400
#define SIRIUS_O_NONBLOCK   0x0800   /* a read with nothing to return gives EAGAIN */
#define SIRIUS_O_DIRECTORY  0x10000
#define SIRIUS_O_CLOEXEC    0x80000  /* the descriptor does not survive execve */

/* lseek() whence */
#define SIRIUS_SEEK_SET 0
#define SIRIUS_SEEK_CUR 1
#define SIRIUS_SEEK_END 2

/* st_mode file types */
#define SIRIUS_S_IFMT   0170000
#define SIRIUS_S_IFDIR  0040000
#define SIRIUS_S_IFCHR  0020000
#define SIRIUS_S_IFBLK  0060000
#define SIRIUS_S_IFREG  0100000
#define SIRIUS_S_IFIFO  0010000
#define SIRIUS_S_IFSOCK 0140000

struct sirius_stat {
    uint64_t st_ino;
    uint64_t st_size;
    uint64_t st_blocks;     /* 512-byte units */
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_blksize;
    uint32_t st_dev;
};

/* d_type values */
#define SIRIUS_DT_FIFO 1
#define SIRIUS_DT_CHR  2
#define SIRIUS_DT_DIR  4
#define SIRIUS_DT_BLK  6
#define SIRIUS_DT_REG  8
#define SIRIUS_DT_SOCK 12

#define SIRIUS_NAME_MAX 255
#define SIRIUS_PATH_MAX 1024

struct sirius_dirent {
    uint64_t d_ino;
    uint32_t d_type;
    char     d_name[SIRIUS_NAME_MAX + 1];
};

/* mmap() */
#define SIRIUS_PROT_NONE   0x0
#define SIRIUS_PROT_READ   0x1
#define SIRIUS_PROT_WRITE  0x2
#define SIRIUS_PROT_EXEC   0x4
#define SIRIUS_MAP_SHARED     0x01
#define SIRIUS_MAP_PRIVATE    0x02
#define SIRIUS_MAP_FIXED      0x10
#define SIRIUS_MAP_ANONYMOUS  0x20
#define SIRIUS_MAP_FAILED  ((void*)-1)

/* flock(): an advisory lock on a file, held by an open file (and so shared
 * by descriptors duplicated or inherited from it) and released when that is
 * closed. */
#define SIRIUS_LOCK_SH  1   /* shared: any number of holders */
#define SIRIUS_LOCK_EX  2   /* exclusive */
#define SIRIUS_LOCK_NB  4   /* or-ed in: EAGAIN instead of waiting */
#define SIRIUS_LOCK_UN  8   /* release */

/* waitpid() */
#define SIRIUS_WNOHANG 1
/* Status encoding, as on Linux: exited normally with code c -> c << 8;
 * killed by signal s -> s. */
#define SIRIUS_WIFEXITED(s)   (((s) & 0x7f) == 0)
#define SIRIUS_WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define SIRIUS_WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define SIRIUS_WTERMSIG(s)    ((s) & 0x7f)

#endif
