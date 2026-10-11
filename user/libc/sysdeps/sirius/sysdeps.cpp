// mlibc's system-dependent layer for Sirius OS: every libc function that
// needs the kernel comes through one of the Sysdeps below.
//
// Each returns 0 or an errno value, never -1; results travel through the
// out-parameters. The kernel reports errors as -errno in rax, with Linux's
// numbering, so they pass straight through.
#include <bits/ensure.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mlibc/all-sysdeps.hpp>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include <sirius/syscall.h>

namespace {

long syscall(long number, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
	long result;
	register long r10 asm("r10") = d;
	register long r8 asm("r8") = e;
	register long r9 asm("r9") = f;
	asm volatile("syscall"
	             : "=a"(result)
	             : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
	             : "rcx", "r11", "memory");
	return result;
}

template<typename T>
long arg(T value) {
	return (long)value;
}

bool failed(long result) {
	return result < 0 && result > -4096;
}

// For calls with nothing to return but success.
int status_of(long result) {
	return failed(result) ? static_cast<int>(-result) : 0;
}

void convert_stat(const sirius_stat &in, struct stat *out) {
	memset(out, 0, sizeof(*out));
	out->st_dev = in.st_dev;
	out->st_ino = in.st_ino;
	out->st_nlink = in.st_nlink;
	out->st_mode = in.st_mode;
	out->st_size = static_cast<off_t>(in.st_size);
	out->st_blksize = in.st_blksize;
	out->st_blocks = static_cast<blkcnt_t>(in.st_blocks);
	// No timestamps: the kernel has no clock to take them from yet.
}

// For calls that return a new descriptor or a count through `out`.
template<typename T>
int value_of(long result, T *out) {
	if (failed(result))
		return static_cast<int>(-result);
	*out = static_cast<T>(result);
	return 0;
}

// The kernel reads and writes the first 64 bits of a signal set: there are
// 64 signals.
long sigset_word(const sigset_t *set) {
	return arg(set);
}

int stat_path(const char *path, sirius_stat &out) {
	return status_of(syscall(SYS_STAT, arg(path), arg(&out)));
}

// The kernel resolves paths against the working directory only.
bool is_cwd(int dirfd) {
	return dirfd == AT_FDCWD;
}

} // namespace

namespace mlibc {

// --- diagnostics ----------------------------------------------------------------------

void Sysdeps<LibcLog>::operator()(const char *message) {
	// One write, so that another process's output cannot split the line.
	char line[512];
	size_t length = strlen(message);
	if (length > sizeof(line) - 1)
		length = sizeof(line) - 1;
	memcpy(line, message, length);
	line[length++] = '\n';
	syscall(SYS_WRITE, 2, arg(line), arg(length));
}

void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("mlibc: panic");
	// There are no signals to die by; 134 is what a shell reports for SIGABRT.
	sysdep<Exit>(134);
}

// --- memory and threads ---------------------------------------------------------------

int Sysdeps<TcbSet>::operator()(void *pointer) {
	return status_of(syscall(SYS_TCB_SET, arg(pointer)));
}

int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
	const long result = syscall(SYS_MMAP, arg(hint), arg(size), prot, flags, fd, offset);
	if (failed(result))
		return static_cast<int>(-result);
	*window = reinterpret_cast<void *>(result);
	return 0;
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
	return status_of(syscall(SYS_MUNMAP, arg(pointer), arg(size)));
}

// A shared mapping is the object's own pages, not a copy of them: there is
// never anything to write back. (libwayland's server calls this before it
// moves a client's memory pool to a larger mapping.)
int Sysdeps<Msync>::operator()(void *address, size_t, int flags) {
	if (reinterpret_cast<uintptr_t>(address) & 4095)
		return EINVAL;
	if ((flags & MS_SYNC) && (flags & MS_ASYNC))
		return EINVAL;
	return 0;
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	return sysdep<VmMap>(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, pointer);
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
	return sysdep<VmUnmap>(pointer, size);
}

// A futex is a word of the program's own that threads sleep on: wait returns
// when another thread has changed the word and said so with wake. Everything
// that blocks in a threaded program -- mutexes, condition variables, joining
// a thread -- is built on these two.
int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const struct timespec *time) {
	// The time is how long to wait, not when to stop.
	long timeout = -1;
	if (time) {
		if (time->tv_sec < 0 || time->tv_nsec < 0)
			return ETIMEDOUT;
		timeout = time->tv_sec > 4000000000L ? -1 : time->tv_sec * 1000000000L + time->tv_nsec;
	}
	return status_of(syscall(SYS_FUTEX_WAIT, arg(pointer), static_cast<unsigned int>(expected), timeout));
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool all) {
	return status_of(syscall(SYS_FUTEX_WAKE, arg(pointer), all ? INT_MAX : 1));
}

// A thread's stack: the top of a new mapping, with a guard below it that
// nothing may touch, so running off the end faults instead of scribbling on
// whatever is mapped next. The new thread starts in __sirius_thread_entry
// (crt-x86_64/thread-entry.S), which takes its function and argument from
// the top of this stack.
int Sysdeps<PrepareStack>::operator()(void **stack, void *entry, void *user_arg, void *, size_t *stack_size,
		size_t *guard_size, void **stack_base) {
	if (!*stack_size)
		*stack_size = 0x200000;
	*stack_size = (*stack_size + 0xfff) & ~size_t(0xfff);
	*guard_size = (*guard_size + 0xfff) & ~size_t(0xfff);

	void *map;
	if (*stack) {
		map = *stack;
		*guard_size = 0;
	} else {
		// Reserve all of it out of reach, then make the stack itself usable.
		if (sysdep<VmMap>(nullptr, *stack_size + *guard_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, &map))
			return EAGAIN;
		void *usable;
		if (sysdep<VmMap>(static_cast<char *>(map) + *guard_size, *stack_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0, &usable))
			return EAGAIN;
	}

	*stack_base = map;
	auto sp = reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(map) + *guard_size + *stack_size);
	*--sp = reinterpret_cast<uintptr_t>(user_arg);
	*--sp = reinterpret_cast<uintptr_t>(entry);
	*stack = sp;
	return 0;
}

extern "C" void __sirius_thread_entry();

// The kernel starts the thread where it is told, on the stack it is given,
// with the thread pointer already set: nothing is copied, as fork would.
int Sysdeps<Clone>::operator()(void *tcb, pid_t *pid_out, void *stack) {
	const long result = syscall(SYS_THREAD_CREATE, arg(&__sirius_thread_entry), arg(stack), arg(tcb));
	if (failed(result))
		return static_cast<int>(-result);
	if (pid_out)
		*pid_out = static_cast<pid_t>(result);
	return 0;
}

void Sysdeps<ThreadExit>::operator()() {
	syscall(SYS_THREAD_EXIT);
	__builtin_unreachable();
}

// --- processes ------------------------------------------------------------------------

void Sysdeps<Exit>::operator()(int status) {
	syscall(SYS_EXIT, status);
	__builtin_unreachable();
}

int Sysdeps<Fork>::operator()(pid_t *child) {
	const long result = syscall(SYS_FORK);
	if (failed(result))
		return static_cast<int>(-result);
	*child = static_cast<pid_t>(result);
	return 0;
}

int Sysdeps<Execve>::operator()(const char *path, char *const argv[], char *const envp[]) {
	return status_of(syscall(SYS_EXECVE, arg(path), arg(argv), arg(envp)));
}

// No resource accounting: `ru` is left as the caller passed it.
int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, struct rusage *, pid_t *ret_pid) {
	const long result = syscall(SYS_WAITPID, pid, arg(status), flags);
	if (failed(result))
		return static_cast<int>(-result);
	*ret_pid = static_cast<pid_t>(result);
	return 0;
}

pid_t Sysdeps<GetPid>::operator()() {
	return static_cast<pid_t>(syscall(SYS_GETPID));
}

pid_t Sysdeps<GetPpid>::operator()() {
	return static_cast<pid_t>(syscall(SYS_GETPPID));
}

void Sysdeps<Yield>::operator()() {
	syscall(SYS_YIELD);
}

// --- time -----------------------------------------------------------------------------

// Every clock is the time since boot: there is no real-time clock driver, so
// CLOCK_REALTIME starts at the epoch on each boot.
int Sysdeps<ClockGet>::operator()(int clock, time_t *secs, long *nanos) {
	const long result = syscall(SYS_CLOCK_GET, clock);
	if (failed(result))
		return static_cast<int>(-result);
	*secs = result / 1000000000;
	*nanos = result % 1000000000;
	return 0;
}

// A signal handler interrupts a sleep; what was left of it is reported back.
int Sysdeps<Sleep>::operator()(time_t *secs, long *nanos) {
	const long wanted = static_cast<long>(*secs) * 1000000000 + *nanos;
	const long start = syscall(SYS_CLOCK_GET, CLOCK_MONOTONIC);
	const long result = syscall(SYS_SLEEP_MS, (wanted + 999999) / 1000000);
	long left = 0;
	if (failed(result)) {
		const long slept = syscall(SYS_CLOCK_GET, CLOCK_MONOTONIC) - start;
		left = slept < wanted ? wanted - slept : 0;
	}
	*secs = left / 1000000000;
	*nanos = left % 1000000000;
	return status_of(result);
}

// --- files ----------------------------------------------------------------------------

int Sysdeps<Open>::operator()(const char *pathname, int flags, mode_t mode, int *fd) {
	const long result = syscall(SYS_OPEN, arg(pathname), flags, mode);
	if (failed(result))
		return static_cast<int>(-result);
	*fd = static_cast<int>(result);
	return 0;
}

int Sysdeps<Openat>::operator()(int dirfd, const char *path, int flags, mode_t mode, int *fd) {
	if (!is_cwd(dirfd) && path[0] != '/')
		return ENOSYS;
	return sysdep<Open>(path, flags, mode, fd);
}

int Sysdeps<Close>::operator()(int fd) {
	return status_of(syscall(SYS_CLOSE, fd));
}

// No descriptor flags exist (nothing is close-on-exec), so `flags` has
// nothing to set.
int Sysdeps<Dup>::operator()(int fd, int, int *newfd) {
	const long result = syscall(SYS_DUP, fd);
	if (failed(result))
		return static_cast<int>(-result);
	*newfd = static_cast<int>(result);
	return 0;
}

int Sysdeps<Dup2>::operator()(int fd, int, int newfd) {
	return status_of(syscall(SYS_DUP2, fd, newfd));
}

int Sysdeps<Read>::operator()(int fd, void *buf, size_t count, ssize_t *bytes_read) {
	const long result = syscall(SYS_READ, fd, arg(buf), arg(count));
	if (failed(result))
		return static_cast<int>(-result);
	*bytes_read = result;
	return 0;
}

int Sysdeps<Write>::operator()(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
	const long result = syscall(SYS_WRITE, fd, arg(buf), arg(count));
	if (failed(result))
		return static_cast<int>(-result);
	*bytes_written = result;
	return 0;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	const long result = syscall(SYS_LSEEK, fd, offset, whence);
	if (failed(result))
		return static_cast<int>(-result);
	*new_offset = result;
	return 0;
}

int Sysdeps<Ioctl>::operator()(int fd, unsigned long request, void *arg, int *result) {
	const long r = syscall(SYS_IOCTL, fd, static_cast<long>(request), reinterpret_cast<long>(arg));
	if (failed(r))
		return static_cast<int>(-r);
	*result = static_cast<int>(r);
	return 0;
}

int Sysdeps<Stat>::operator()(fsfd_target fsfdt, int fd, const char *path, int, struct stat *statbuf) {
	sirius_stat st;
	long result;
	switch (fsfdt) {
	case fsfd_target::fd:
		result = syscall(SYS_FSTAT, fd, arg(&st));
		break;
	case fsfd_target::fd_path:
		if (!is_cwd(fd) && path[0] != '/')
			return ENOSYS;
		[[fallthrough]];
	case fsfd_target::path:
		result = syscall(SYS_STAT, arg(path), arg(&st));
		break;
	default:
		return EINVAL;
	}
	if (failed(result))
		return static_cast<int>(-result);
	convert_stat(st, statbuf);
	return 0;
}

// Character devices are terminals: a pseudo-terminal's either end, and the
// console, which is not one but is where a program's output is meant to be
// read as it comes -- calling it a terminal makes stdout line-buffered there.
int Sysdeps<Isatty>::operator()(int fd) {
	sirius_stat st;
	const long result = syscall(SYS_FSTAT, fd, arg(&st));
	if (failed(result))
		return static_cast<int>(-result);
	return (st.st_mode & SIRIUS_S_IFMT) == SIRIUS_S_IFCHR ? 0 : ENOTTY;
}

// No permission bits exist, so anything that is there may be used.
int Sysdeps<Access>::operator()(const char *path, int) {
	sirius_stat st;
	return stat_path(path, st);
}

// --- directories ----------------------------------------------------------------------

int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
	return sysdep<Open>(path, O_RDONLY | O_DIRECTORY, 0, handle);
}

// The kernel hands out one entry per call; pack as many as fit.
int Sysdeps<ReadEntries>::operator()(int handle, void *buffer, size_t max_size, size_t *bytes_read) {
	size_t used = 0;
	while (max_size - used >= sizeof(struct dirent)) {
		sirius_dirent entry;
		const long result = syscall(SYS_READDIR, handle, arg(&entry));
		if (failed(result)) {
			if (used)
				break;      // report what was read; the error will come back next time
			return static_cast<int>(-result);
		}
		if (result == 0)
			break;

		auto *out = reinterpret_cast<struct dirent *>(static_cast<char *>(buffer) + used);
		memset(out, 0, sizeof(*out));
		out->d_ino = entry.d_ino;
		out->d_reclen = sizeof(*out);
		out->d_type = static_cast<unsigned char>(entry.d_type);   // the DT_ values agree
		strncpy(out->d_name, entry.d_name, sizeof(out->d_name) - 1);
		used += sizeof(*out);
	}
	*bytes_read = used;
	return 0;
}

int Sysdeps<Mkdir>::operator()(const char *path, mode_t mode) {
	return status_of(syscall(SYS_MKDIR, arg(path), mode));
}

// The kernel's unlink removes files and empty directories alike. POSIX
// keeps the two apart, so look first.
int Sysdeps<Unlinkat>::operator()(int dirfd, const char *path, int flags) {
	if (!is_cwd(dirfd) && path[0] != '/')
		return ENOSYS;
	sirius_stat st;
	if (int e = stat_path(path, st); e)
		return e;
	const bool directory = (st.st_mode & SIRIUS_S_IFMT) == SIRIUS_S_IFDIR;
	if (flags & AT_REMOVEDIR) {
		if (!directory)
			return ENOTDIR;
	} else if (directory) {
		return EISDIR;
	}
	return status_of(syscall(SYS_UNLINK, arg(path)));
}

int Sysdeps<Rmdir>::operator()(const char *path) {
	return sysdep<Unlinkat>(AT_FDCWD, path, AT_REMOVEDIR);
}

int Sysdeps<Chdir>::operator()(const char *path) {
	return status_of(syscall(SYS_CHDIR, arg(path)));
}

int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
	const long result = syscall(SYS_GETCWD, arg(buffer), arg(size));
	// The kernel calls a buffer that is too small an invalid argument.
	if (result == -EINVAL)
		return ERANGE;
	return status_of(result);
}

// --- descriptors ----------------------------------------------------------------------

int Sysdeps<Pipe>::operator()(int *fds, int flags) {
	return status_of(syscall(SYS_PIPE, arg(fds), flags));
}

int Sysdeps<Fcntl>::operator()(int fd, int request, va_list args, int *result) {
	// Every request the kernel knows takes an int or nothing. File locks
	// have nobody to exclude: they succeed.
	if (request == F_GETLK || request == F_SETLK || request == F_SETLKW) {
		*result = 0;
		return 0;
	}
	return value_of(syscall(SYS_FCNTL, fd, request, va_arg(args, int)), result);
}

int Sysdeps<Pread>::operator()(int fd, void *buf, size_t n, off_t off, ssize_t *bytes_read) {
	return value_of(syscall(SYS_PREAD, fd, arg(buf), arg(n), off), bytes_read);
}

int Sysdeps<Pwrite>::operator()(int fd, const void *buf, size_t n, off_t off, ssize_t *bytes_written) {
	return value_of(syscall(SYS_PWRITE, fd, arg(buf), arg(n), off), bytes_written);
}

int Sysdeps<Ftruncate>::operator()(int fd, size_t size) {
	return status_of(syscall(SYS_FTRUNCATE, fd, arg(size)));
}

// No filesystem reserves space ahead of a write -- tmpfs and memfds allocate
// as they are sized, FAT as it is written -- so all there is to do is make
// a file that is too short long enough. Shared memory pools are sized this
// way (libwayland-cursor's, for one).
int Sysdeps<Fallocate>::operator()(int fd, off_t offset, size_t size) {
	if (offset < 0)
		return EINVAL;
	sirius_stat st;
	if (int e = status_of(syscall(SYS_FSTAT, fd, arg(&st))); e)
		return e;
	const uint64_t end = static_cast<uint64_t>(offset) + size;
	if (end <= st.st_size)
		return 0;
	return sysdep<Ftruncate>(fd, static_cast<size_t>(end));
}

int Sysdeps<MemfdCreate>::operator()(const char *name, int flags, int *fd) {
	return value_of(syscall(SYS_MEMFD_CREATE, arg(name), flags), fd);
}

int Sysdeps<Flock>::operator()(int fd, int options) {
	return status_of(syscall(SYS_FLOCK, fd, options));
}

// Every write reaches the disk before it returns (ADR 0007).
int Sysdeps<Fsync>::operator()(int) {
	return 0;
}

// --- waiting --------------------------------------------------------------------------

int Sysdeps<Poll>::operator()(struct pollfd *fds, nfds_t count, int timeout, int *num_events) {
	return value_of(syscall(SYS_POLL, arg(fds), arg(count), timeout), num_events);
}

int Sysdeps<EpollCreate>::operator()(int flags, int *fd) {
	return value_of(syscall(SYS_EPOLL_CREATE, flags), fd);
}

int Sysdeps<EpollCtl>::operator()(int epfd, int mode, int fd, struct epoll_event *ev) {
	return status_of(syscall(SYS_EPOLL_CTL, epfd, mode, fd, arg(ev)));
}

int Sysdeps<EpollPwait>::operator()(int epfd, struct epoll_event *ev, int n, int timeout, const sigset_t *sigmask, int *raised) {
	return value_of(syscall(SYS_EPOLL_WAIT, epfd, arg(ev), n, timeout, sigset_word(sigmask)), raised);
}

int Sysdeps<TimerfdCreate>::operator()(int clockid, int flags, int *fd) {
	return value_of(syscall(SYS_TIMERFD_CREATE, clockid, flags), fd);
}

int Sysdeps<TimerfdSettime>::operator()(int fd, int flags, const struct itimerspec *value, struct itimerspec *oldvalue) {
	return status_of(syscall(SYS_TIMERFD_SETTIME, fd, flags, arg(value), arg(oldvalue)));
}

int Sysdeps<SignalfdCreate>::operator()(const sigset_t *mask, int flags, int *fd) {
	return value_of(syscall(SYS_SIGNALFD, -1, sigset_word(mask), flags), fd);
}

int Sysdeps<EventfdCreate>::operator()(unsigned int initval, int flags, int *fd) {
	return value_of(syscall(SYS_EVENTFD, arg(initval), flags), fd);
}

// --- signals --------------------------------------------------------------------------

extern "C" void __sirius_signal_restorer();

// The first four fields of mlibc's struct sigaction and the kernel's are laid
// out alike; the mask differs in size (1024 bits against 64).
int Sysdeps<Sigaction>::operator()(int sig, const struct sigaction *__restrict act, struct sigaction *__restrict oldact) {
	sirius_sigaction kernel_act{}, kernel_old{};
	if (act) {
		kernel_act.handler = reinterpret_cast<uint64_t>(act->sa_sigaction);
		kernel_act.flags = static_cast<uint32_t>(act->sa_flags) | SIRIUS_SA_RESTORER;
		// A handler returns into this, which makes the sigreturn system call.
		kernel_act.restorer = reinterpret_cast<uint64_t>(&__sirius_signal_restorer);
		memcpy(&kernel_act.mask, &act->sa_mask, sizeof(kernel_act.mask));
	}
	const long result = syscall(SYS_SIGACTION, sig, act ? arg(&kernel_act) : 0, oldact ? arg(&kernel_old) : 0);
	if (failed(result))
		return static_cast<int>(-result);
	if (oldact) {
		memset(oldact, 0, sizeof(*oldact));
		oldact->sa_sigaction = reinterpret_cast<void (*)(int, siginfo_t *, void *)>(kernel_old.handler);
		oldact->sa_flags = static_cast<int>(kernel_old.flags & ~SIRIUS_SA_RESTORER);
		memcpy(&oldact->sa_mask, &kernel_old.mask, sizeof(kernel_old.mask));
	}
	return 0;
}

int Sysdeps<Sigprocmask>::operator()(int how, const sigset_t *__restrict set, sigset_t *__restrict retrieve) {
	if (retrieve)
		memset(retrieve, 0, sizeof(*retrieve));
	return status_of(syscall(SYS_SIGPROCMASK, how, sigset_word(set), sigset_word(retrieve)));
}

// The kernel keeps a mask per thread: this is the calling thread's.
int Sysdeps<ThreadSigmask>::operator()(int how, const sigset_t *__restrict set, sigset_t *__restrict retrieve) {
	return sysdep<Sigprocmask>(how, set, retrieve);
}

int Sysdeps<Sigpending>::operator()(sigset_t *set) {
	memset(set, 0, sizeof(*set));
	return status_of(syscall(SYS_SIGPENDING, sigset_word(set)));
}

int Sysdeps<Sigsuspend>::operator()(const sigset_t *set) {
	return status_of(syscall(SYS_SIGSUSPEND, sigset_word(set)));
}

// pause() is sigsuspend() with the mask left as it is.
int Sysdeps<Pause>::operator()() {
	sigset_t current;
	if (int e = sysdep<Sigprocmask>(SIG_BLOCK, nullptr, &current); e)
		return e;
	return sysdep<Sigsuspend>(&current);
}

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
	return status_of(syscall(SYS_KILL, pid, sig));
}

// A signal for one thread. Threads are only ever signalled within their own
// process, so the process id is not needed.
int Sysdeps<Tgkill>::operator()(int, int tid, int sig) {
	return status_of(syscall(SYS_TKILL, tid, sig));
}

int Sysdeps<SetItimer>::operator()(int which, const struct itimerval *new_value, struct itimerval *old_value) {
	return status_of(syscall(SYS_SETITIMER, which, arg(new_value), arg(old_value)));
}

// waitid() in terms of waitpid(): the same child, reported as a siginfo.
int Sysdeps<Waitid>::operator()(idtype_t idtype, id_t id, siginfo_t *info, int options) {
	if (idtype != P_PID && idtype != P_ALL)
		return EINVAL;
	if (!(options & WEXITED))
		return EINVAL;
	int status = 0;
	pid_t pid = 0;
	if (int e = sysdep<Waitpid>(idtype == P_PID ? static_cast<pid_t>(id) : -1, &status, options & WNOHANG, nullptr, &pid); e)
		return e;
	memset(info, 0, sizeof(*info));
	if (pid == 0)
		return 0;   // WNOHANG, and nobody has exited: si_pid stays 0
	info->si_signo = SIGCHLD;
	info->si_pid = pid;
	if (WIFEXITED(status)) {
		info->si_code = CLD_EXITED;
		info->si_status = WEXITSTATUS(status);
	} else {
		info->si_code = CLD_KILLED;
		info->si_status = WTERMSIG(status);
	}
	return 0;
}

// --- sockets --------------------------------------------------------------------------

int Sysdeps<Socket>::operator()(int family, int type, int protocol, int *fd) {
	return value_of(syscall(SYS_SOCKET, family, type, protocol), fd);
}

int Sysdeps<Socketpair>::operator()(int domain, int type_and_flags, int proto, int *fds) {
	return status_of(syscall(SYS_SOCKETPAIR, domain, type_and_flags, proto, arg(fds)));
}

int Sysdeps<Bind>::operator()(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
	return status_of(syscall(SYS_BIND, fd, arg(addr_ptr), arg(addr_length)));
}

int Sysdeps<Listen>::operator()(int fd, int backlog) {
	return status_of(syscall(SYS_LISTEN, fd, backlog));
}

int Sysdeps<Connect>::operator()(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
	return status_of(syscall(SYS_CONNECT, fd, arg(addr_ptr), arg(addr_length)));
}

// A unix stream socket's peer has no address worth reporting: the family,
// and nothing after it.
int Sysdeps<Accept>::operator()(int fd, int *newfd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags) {
	if (int e = value_of(syscall(SYS_ACCEPT, fd, flags), newfd); e)
		return e;
	if (addr_ptr && addr_length) {
		const sa_family_t family = AF_UNIX;
		memcpy(addr_ptr, &family, *addr_length < sizeof(family) ? *addr_length : sizeof(family));
		*addr_length = sizeof(family);
	}
	return 0;
}

int Sysdeps<Shutdown>::operator()(int sockfd, int how) {
	return status_of(syscall(SYS_SHUTDOWN, sockfd, how));
}

int Sysdeps<GetSockopt>::operator()(int fd, int layer, int number, void *__restrict buffer, socklen_t *__restrict size) {
	return status_of(syscall(SYS_GETSOCKOPT, fd, layer, number, arg(buffer), arg(size)));
}

// Buffer sizes, credential passing and the like: nothing a unix socket here
// lets a program tune, and nothing it loses by asking.
int Sysdeps<SetSockopt>::operator()(int, int layer, int, const void *, socklen_t) {
	return layer == SOL_SOCKET ? 0 : ENOPROTOOPT;
}

namespace {

// mlibc's msghdr has two ints of padding that programs leave uninitialised;
// the kernel's has full-width fields there.
sirius_msghdr kernel_message(const struct msghdr *hdr) {
	sirius_msghdr message{};
	message.iov = reinterpret_cast<uint64_t>(hdr->msg_iov);
	message.iov_count = static_cast<uint64_t>(hdr->msg_iovlen);
	message.control = reinterpret_cast<uint64_t>(hdr->msg_control);
	message.control_length = hdr->msg_controllen;
	return message;
}

} // namespace

int Sysdeps<MsgSend>::operator()(int fd, const struct msghdr *hdr, int flags, ssize_t *length) {
	sirius_msghdr message = kernel_message(hdr);
	return value_of(syscall(SYS_SENDMSG, fd, arg(&message), flags), length);
}

int Sysdeps<MsgRecv>::operator()(int fd, struct msghdr *hdr, int flags, ssize_t *length) {
	sirius_msghdr message = kernel_message(hdr);
	if (int e = value_of(syscall(SYS_RECVMSG, fd, arg(&message), flags), length); e)
		return e;
	hdr->msg_controllen = static_cast<socklen_t>(message.control_length);
	hdr->msg_flags = message.flags;
	hdr->msg_namelen = 0;
	return 0;
}

// --- identity -------------------------------------------------------------------------
// One user, who is root; one machine, which is this one.

uid_t Sysdeps<GetUid>::operator()() { return 0; }
uid_t Sysdeps<GetEuid>::operator()() { return 0; }
gid_t Sysdeps<GetGid>::operator()() { return 0; }
gid_t Sysdeps<GetEgid>::operator()() { return 0; }

pid_t Sysdeps<GetTid>::operator()() {
	return static_cast<pid_t>(syscall(SYS_GETTID));
}

int Sysdeps<Umask>::operator()(mode_t, mode_t *old) {
	*old = 022;
	return 0;
}

int Sysdeps<GetHostname>::operator()(char *buffer, size_t bufsize) {
	strncpy(buffer, "sirius", bufsize);
	return 0;
}

// --- sessions, process groups and terminals --------------------------------------------

int Sysdeps<SetSid>::operator()(pid_t *sid) {
	const long result = syscall(SYS_SETSID);
	if (failed(result))
		return static_cast<int>(-result);
	*sid = static_cast<pid_t>(result);
	return 0;
}

int Sysdeps<GetSid>::operator()(pid_t pid, pid_t *sid) {
	const long result = syscall(SYS_GETSID, pid);
	if (failed(result))
		return static_cast<int>(-result);
	*sid = static_cast<pid_t>(result);
	return 0;
}

int Sysdeps<SetPgid>::operator()(pid_t pid, pid_t pgid) {
	if (pid < 0 || pgid < 0)
		return EINVAL;
	return status_of(syscall(SYS_SETPGID, pid, pgid));
}

int Sysdeps<GetPgid>::operator()(pid_t pid, pid_t *pgid) {
	const long result = syscall(SYS_GETPGID, pid);
	if (failed(result))
		return static_cast<int>(-result);
	*pgid = static_cast<pid_t>(result);
	return 0;
}

// Both ends at once: there is no /dev/ptmx to open, and no name for the
// slave to be found by.
int Sysdeps<Openpty>::operator()(int *mfd, int *sfd, char *name, const struct termios *ios, const struct winsize *win) {
	static_assert(sizeof(struct termios) == sizeof(sirius_termios), "the kernel's termios is the C library's");
	static_assert(sizeof(struct winsize) == sizeof(sirius_winsize), "and so is its window size");
	int fds[2];
	if (int e = status_of(syscall(SYS_OPENPTY, arg(fds), arg(ios), arg(win))); e)
		return e;
	*mfd = fds[0];
	*sfd = fds[1];
	if (name)
		strcpy(name, "pty");
	return 0;
}

int Sysdeps<Tcgetattr>::operator()(int fd, struct termios *attr) {
	return status_of(syscall(SYS_IOCTL, fd, SIRIUS_TCGETS, arg(attr)));
}

// TCSANOW, TCSADRAIN and TCSAFLUSH are 0, 1 and 2: the three requests in order.
int Sysdeps<Tcsetattr>::operator()(int fd, int when, const struct termios *attr) {
	if (when < TCSANOW || when > TCSAFLUSH)
		return EINVAL;
	return status_of(syscall(SYS_IOCTL, fd, SIRIUS_TCSETS + when, arg(attr)));
}

int Sysdeps<Tcflush>::operator()(int fd, int queue) {
	return status_of(syscall(SYS_IOCTL, fd, SIRIUS_TCFLSH, queue));
}

// Output reaches the master as it is written: there is nothing to wait for,
// once the descriptor is known to be a terminal.
int Sysdeps<Tcdrain>::operator()(int fd) {
	struct termios scratch;
	return sysdep<Tcgetattr>(fd, &scratch);
}

int Sysdeps<Tcgetwinsize>::operator()(int fd, struct winsize *size) {
	return status_of(syscall(SYS_IOCTL, fd, SIRIUS_TIOCGWINSZ, arg(size)));
}

int Sysdeps<Tcsetwinsize>::operator()(int fd, const struct winsize *size) {
	return status_of(syscall(SYS_IOCTL, fd, SIRIUS_TIOCSWINSZ, arg(size)));
}

int Sysdeps<Uname>::operator()(struct utsname *buf) {
	memset(buf, 0, sizeof(*buf));
	strcpy(buf->sysname, "Sirius");
	strcpy(buf->nodename, "sirius");
	strcpy(buf->release, "0.8");
	strcpy(buf->version, "M8");
	strcpy(buf->machine, "x86_64");
	return 0;
}

} // namespace mlibc
