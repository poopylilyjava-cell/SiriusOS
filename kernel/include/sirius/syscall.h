/* Sirius system call ABI, shared by the kernel and user space.
 *
 * Calling convention (the x86-64 SYSCALL instruction):
 *   rax  system call number
 *   rdi, rsi, rdx, r10, r8, r9  arguments 1-6  (r10, not rcx: SYSCALL
 *                                               overwrites rcx with the
 *                                               return address)
 *   rax  return value; values in [-4095, -1] are -errno (see errno.h)
 *   rcx, r11  clobbered; every other register is preserved
 *
 * Plain C: mlibc's Sirius sysdeps (user/libc/sysdeps/sirius) include this
 * file unchanged, and it is installed as <sirius/syscall.h>. A number, once
 * a released libc uses it, is never reused. */
#ifndef SIRIUS_SYSCALL_H
#define SIRIUS_SYSCALL_H

#include <sirius/errno.h>
#include <sirius/fs.h>
#include <sirius/poll.h>
#include <sirius/signal.h>
#include <sirius/socket.h>
#include <sirius/time.h>
#include <sirius/tty.h>

#define SYS_WRITE     0   /* write(fd, buffer, length) -> bytes written           */
#define SYS_EXIT      1   /* exit(status) -> does not return                      */
#define SYS_YIELD     2   /* yield() -> 0                                         */
#define SYS_SLEEP_MS  3   /* sleep_ms(milliseconds) -> 0                          */
#define SYS_GETCPU    4   /* getcpu() -> index of the CPU running the caller      */
#define SYS_GETPID    5   /* getpid() -> process id                               */
#define SYS_OPEN      6   /* open(path, flags, mode) -> fd                        */
#define SYS_READ      7   /* read(fd, buffer, length) -> bytes read, 0 at EOF     */
#define SYS_CLOSE     8   /* close(fd) -> 0                                       */
#define SYS_STAT      9   /* stat(path, struct sirius_stat*) -> 0                 */
#define SYS_FSTAT     10  /* fstat(fd, struct sirius_stat*) -> 0                  */
#define SYS_LSEEK     11  /* lseek(fd, offset, whence) -> new offset              */
#define SYS_MMAP      12  /* mmap(addr, length, prot, flags, fd, offset) -> addr; MAP_SHARED only of devices */
#define SYS_MUNMAP    13  /* munmap(addr, length) -> 0                            */
#define SYS_FORK      14  /* fork() -> child pid in the parent, 0 in the child    */
#define SYS_EXECVE    15  /* execve(path, argv, envp) -> does not return on success */
#define SYS_WAITPID   16  /* waitpid(pid, int* status, options) -> pid, 0 (WNOHANG) */
#define SYS_READDIR   17  /* readdir(fd, struct sirius_dirent*) -> 1, or 0 at end */
#define SYS_MKDIR     18  /* mkdir(path, mode) -> 0                               */
#define SYS_UNLINK    19  /* unlink(path) -> 0; also removes empty directories    */
#define SYS_GETPPID   20  /* getppid() -> parent pid, 0 if orphaned               */
#define SYS_CHDIR     21  /* chdir(path) -> 0                                     */
#define SYS_GETCWD    22  /* getcwd(buffer, size) -> length                       */
#define SYS_TCB_SET   23  /* tcb_set(address) -> 0; the thread's FS base          */
#define SYS_CLOCK_GET 24  /* clock_get(clock) -> nanoseconds; see sirius/time.h   */
#define SYS_DUP       25  /* dup(fd) -> the lowest free fd, sharing fd's offset   */
#define SYS_DUP2      26  /* dup2(fd, newfd) -> newfd, closing what it held       */
#define SYS_IOCTL     27  /* ioctl(fd, request, argument) -> device-defined       */
#define SYS_PIPE      28  /* pipe(int fds[2], flags) -> 0; O_CLOEXEC, O_NONBLOCK   */
#define SYS_FCNTL     29  /* fcntl(fd, command, argument); see sirius/poll.h      */
#define SYS_POLL      30  /* poll(struct sirius_pollfd*, count, timeout_ms) -> ready */
#define SYS_KILL      31  /* kill(pid, signal) -> 0; signal 0 only checks the pid */
#define SYS_SIGACTION 32  /* sigaction(signal, const act*, old*) -> 0             */
#define SYS_SIGPROCMASK 33 /* sigprocmask(how, const u64* set, u64* old) -> 0     */
#define SYS_SIGRETURN 34  /* sigreturn(): only from a handler's restorer          */
#define SYS_SIGSUSPEND 35 /* sigsuspend(const u64* mask) -> -EINTR, after a handler ran */
#define SYS_SOCKET    36  /* socket(domain, type, protocol) -> fd                 */
#define SYS_SOCKETPAIR 37 /* socketpair(domain, type, protocol, int fds[2]) -> 0  */
#define SYS_BIND      38  /* bind(fd, const sockaddr*, length) -> 0               */
#define SYS_LISTEN    39  /* listen(fd, backlog) -> 0                             */
#define SYS_ACCEPT    40  /* accept(fd, flags) -> fd; flags as socket()'s type    */
#define SYS_CONNECT   41  /* connect(fd, const sockaddr*, length) -> 0            */
#define SYS_SENDMSG   42  /* sendmsg(fd, const msghdr*, flags) -> bytes sent      */
#define SYS_RECVMSG   43  /* recvmsg(fd, msghdr*, flags) -> bytes; 0 at end of stream */
#define SYS_SHUTDOWN  44  /* shutdown(fd, how) -> 0                               */
#define SYS_GETSOCKOPT 45 /* getsockopt(fd, level, name, value*, u32* length) -> 0 */
#define SYS_MEMFD_CREATE 46 /* memfd_create(name, flags) -> fd of an empty shared memory object */
#define SYS_FTRUNCATE 47  /* ftruncate(fd, size) -> 0                             */
#define SYS_EPOLL_CREATE 48 /* epoll_create(flags) -> fd                          */
#define SYS_EPOLL_CTL 49  /* epoll_ctl(epfd, op, fd, const event*) -> 0           */
#define SYS_EPOLL_WAIT 50 /* epoll_wait(epfd, event*, max, timeout_ms, const u64* sigmask) -> count */
#define SYS_TIMERFD_CREATE 51  /* timerfd_create(clock, flags) -> fd              */
#define SYS_TIMERFD_SETTIME 52 /* timerfd_settime(fd, flags, const itimerspec*, old*) -> 0 */
#define SYS_SIGNALFD  53  /* signalfd(fd or -1, const u64* mask, flags) -> fd     */
#define SYS_EVENTFD   54  /* eventfd(initial, flags) -> fd                        */
#define SYS_SETITIMER 55  /* setitimer(which, const itimerval*, old*) -> 0        */
#define SYS_SIGPENDING 56 /* sigpending(u64* set) -> 0                            */
#define SYS_PREAD     57  /* pread(fd, buffer, length, offset) -> bytes; the file's own offset is untouched */
#define SYS_PWRITE    58  /* pwrite(fd, buffer, length, offset) -> bytes          */
#define SYS_FLOCK     59  /* flock(fd, operation) -> 0; see sirius/fs.h           */
#define SYS_SYSINFO   60  /* sysinfo(struct sirius_sysinfo*) -> 0; see sirius/sysinfo.h */
#define SYS_PROCLIST  61  /* proclist(struct sirius_procinfo*, capacity) -> count  */
#define SYS_THREAD_CREATE 62 /* thread_create(entry, stack, tcb) -> tid: a new thread of this process,
                                starting at `entry` on `stack` with `tcb` as its thread pointer */
#define SYS_THREAD_EXIT 63 /* thread_exit(): ends the calling thread; the process ends with its last */
#define SYS_FUTEX_WAIT 64 /* futex_wait(const u32* word, expected, timeout_ns or -1) -> 0 when woken;
                             EAGAIN if *word != expected, ETIMEDOUT, EINTR */
#define SYS_FUTEX_WAKE 65 /* futex_wake(const u32* word, count) -> threads woken    */
#define SYS_GETTID    66  /* gettid() -> the calling thread's id                   */
#define SYS_TKILL     67  /* tkill(tid, sig) -> 0: a signal for one thread of this process */
#define SYS_SETSID    68  /* setsid() -> the new session's id, the caller's pid; EPERM for a group leader */
#define SYS_GETSID    69  /* getsid(pid, 0 for the caller) -> its session id      */
#define SYS_SETPGID   70  /* setpgid(pid or 0, group or 0) -> 0: move a process (the caller or
                             its child) into a group of its session; 0 means its own pid */
#define SYS_GETPGID   71  /* getpgid(pid, 0 for the caller) -> its process group  */
#define SYS_OPENPTY   72  /* openpty(int fds[2], const sirius_termios* or 0, const sirius_winsize* or 0)
                             -> 0: fds[0] the master of a new pseudo-terminal, fds[1] its slave */

#endif
