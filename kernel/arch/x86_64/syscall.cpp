#include <arch/x86_64/syscall.hpp>
#include <arch/x86_64/clock.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/percpu.hpp>
#include <sched/process.hpp>
#include <sched/sched.hpp>
#include <sched/signal.hpp>
#include <ipc/ipc.hpp>
#include <fs/vfs.hpp>
#include <mm/usercopy.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/syscall.h>
#include <sirius/sysinfo.h>
#include <arch/x86_64/lapic.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <stddef.h>

extern "C" void syscall_entry();
extern "C" [[noreturn]] void syscall_return_from(const SyscallFrame* frame);

static_assert(offsetof(SyscallFrame, rip) == 13 * 8, "SyscallFrame must match syscall.S");

namespace syscall {
namespace {

// RFLAGS bits cleared on entry: TF, IF, DF and AC. Interrupts stay off until
// the stack switch is done; DF must be clear for C code.
constexpr u64 kEntryFlagsMask = (1 << 8) | (1 << 9) | (1 << 10) | (1 << 18);

// Transfers through the kernel are split into pieces of this size, so a huge
// read or write from one process cannot hold the VFS lock for long.
constexpr u64 kIoChunk = 64 * 1024;

constexpr u64 kUserEnd = 0x0000800000000000ULL;   // one past the canonical lower half

i64 error(int code) { return -static_cast<i64>(code); }

sched::Process* self() { return sched::current_process(); }

vfs::File* file_for(u64 fd) { return sched::file_of(fd); }

i64 copy_path(char (&path)[path::kPathMax + 1], u64 address) {
    const i64 n = usercopy::string_from(path, address, sizeof(path));
    if (n == 0) return error(SIRIUS_ENOENT);
    return n < 0 ? n : 0;
}

i64 sys_write(u64 fd, u64 buffer, u64 length) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    if (!usercopy::readable(buffer, length)) return error(SIRIUS_EFAULT);

    u64 done = 0;
    while (done < length) {
        const u64 n = length - done < kIoChunk ? length - done : kIoChunk;
        const i64 written = vfs::write(file, reinterpret_cast<const void*>(buffer + done), n);
        if (written < 0) return done ? static_cast<i64>(done) : written;
        done += static_cast<u64>(written);
        if (static_cast<u64>(written) < n) break;
    }
    return static_cast<i64>(done);
}

i64 sys_read(u64 fd, u64 buffer, u64 length) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    if (!usercopy::writable(buffer, length)) return error(SIRIUS_EFAULT);

    u64 done = 0;
    while (done < length) {
        const u64 n = length - done < kIoChunk ? length - done : kIoChunk;
        const i64 got = vfs::read(file, reinterpret_cast<void*>(buffer + done), n);
        if (got < 0) return done ? static_cast<i64>(done) : got;
        done += static_cast<u64>(got);
        if (static_cast<u64>(got) < n) break;    // end of file, or a short device read
    }
    return static_cast<i64>(done);
}

i64 sys_open(u64 path_address, u64 flags, u64) {
    char path[path::kPathMax + 1];
    i64 status = copy_path(path, path_address);
    if (status < 0) return status;

    sched::Process* process = self();
    vfs::File* file = nullptr;
    status = vfs::open(process->cwd, path, static_cast<u32>(flags), file);
    if (status < 0) return status;
    const i64 fd = sched::install_fd(process, file, (flags & SIRIUS_O_CLOEXEC) ? SIRIUS_FD_CLOEXEC : 0);
    if (fd < 0) vfs::file_close(file);
    return fd;
}

i64 sys_close(u64 fd) {
    vfs::File* file = sched::detach_fd(self(), fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    vfs::file_close(file);
    return 0;
}

// Both descriptors then name one open file description: they share its
// offset, as two descriptors do after fork.
i64 sys_dup(u64 fd) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    const i64 new_fd = sched::install_fd(self(), vfs::file_ref(file));
    if (new_fd < 0) vfs::file_close(file);
    return new_fd;
}

i64 sys_dup2(u64 fd, u64 new_fd) {
    vfs::File* file = file_for(fd);
    if (file == nullptr || new_fd >= sched::kMaxFiles) return error(SIRIUS_EBADF);
    if (fd == new_fd) return static_cast<i64>(new_fd);
    // A duplicate never inherits close-on-exec.
    vfs::file_close(sched::replace_fd(self(), static_cast<u32>(new_fd), vfs::file_ref(file), 0));
    return static_cast<i64>(new_fd);
}

// Descriptor flags belong to the descriptor; status flags to the open file,
// and so to every descriptor that shares it.
i64 sys_fcntl(u64 fd, u64 command, u64 argument) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    sched::Process* process = self();
    switch (command) {
    case SIRIUS_F_DUPFD:
    case SIRIUS_F_DUPFD_CLOEXEC: {
        if (argument >= sched::kMaxFiles) return error(SIRIUS_EINVAL);
        const i64 new_fd = sched::install_fd(process, vfs::file_ref(file),
                                             command == SIRIUS_F_DUPFD_CLOEXEC ? SIRIUS_FD_CLOEXEC : 0, static_cast<u32>(argument));
        if (new_fd < 0) vfs::file_close(file);
        return new_fd;
    }
    case SIRIUS_F_GETFD: return process->fd_flags[fd];
    case SIRIUS_F_SETFD: process->fd_flags[fd] = static_cast<u8>(argument & SIRIUS_FD_CLOEXEC); return 0;
    case SIRIUS_F_GETFL: return file->flags & (SIRIUS_O_ACCMODE | SIRIUS_O_APPEND | SIRIUS_O_NONBLOCK);
    case SIRIUS_F_SETFL: {
        // Only these two can change after open.
        const u32 changeable = SIRIUS_O_APPEND | SIRIUS_O_NONBLOCK;
        __atomic_store_n(&file->flags, (file->flags & ~changeable) | (static_cast<u32>(argument) & changeable), __ATOMIC_RELAXED);
        return 0;
    }
    default: return error(SIRIUS_EINVAL);
    }
}

// Positioned I/O: for files that have positions.
i64 sys_pread(u64 fd, u64 buffer, u64 length, u64 offset) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    if (file->vnode->type() != vfs::NodeType::Regular) return error(SIRIUS_ESPIPE);
    if (length > kIoChunk) length = kIoChunk;
    if (!usercopy::writable(buffer, length)) return error(SIRIUS_EFAULT);
    return vfs::read_at(file, offset, reinterpret_cast<void*>(buffer), length);
}

i64 sys_pwrite(u64 fd, u64 buffer, u64 length, u64 offset) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    if (file->vnode->type() != vfs::NodeType::Regular) return error(SIRIUS_ESPIPE);
    if (length > kIoChunk) length = kIoChunk;
    if (!usercopy::readable(buffer, length)) return error(SIRIUS_EFAULT);
    return vfs::write_at(file, offset, reinterpret_cast<const void*>(buffer), length);
}

i64 sys_flock(u64 fd, u64 operation) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    const int status = vfs::flock(file, static_cast<u32>(operation));
    return status == -SIRIUS_EINTR ? signal::kRestart : status;
}

i64 sys_ftruncate(u64 fd, u64 size) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    return vfs::truncate(file, size);
}

i64 sys_ioctl(u64 fd, u64 request, u64 argument) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    return vfs::ioctl(file, request, argument);
}

i64 sys_stat(u64 path_address, u64 out) {
    char path[path::kPathMax + 1];
    i64 status = copy_path(path, path_address);
    if (status < 0) return status;
    sirius_stat st;
    if ((status = vfs::stat(self()->cwd, path, st)) < 0) return status;
    return usercopy::copy_to(out, &st, sizeof(st));
}

i64 sys_fstat(u64 fd, u64 out) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    sirius_stat st;
    const int status = vfs::fstat(file, st);
    if (status < 0) return status;
    return usercopy::copy_to(out, &st, sizeof(st));
}

i64 sys_lseek(u64 fd, u64 offset, u64 whence) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    return vfs::seek(file, static_cast<i64>(offset), static_cast<int>(whence));
}

i64 sys_readdir(u64 fd, u64 out) {
    vfs::File* file = file_for(fd);
    if (file == nullptr) return error(SIRIUS_EBADF);
    if (!usercopy::writable(out, sizeof(sirius_dirent))) return error(SIRIUS_EFAULT);
    sirius_dirent entry;
    const int status = vfs::readdir(file, entry);
    if (status <= 0) return status;
    return usercopy::copy_to(out, &entry, sizeof(entry)) < 0 ? error(SIRIUS_EFAULT) : 1;
}

i64 sys_mkdir(u64 path_address) {
    char path[path::kPathMax + 1];
    const i64 status = copy_path(path, path_address);
    return status < 0 ? status : vfs::mkdir(self()->cwd, path);
}

i64 sys_unlink(u64 path_address) {
    char path[path::kPathMax + 1];
    const i64 status = copy_path(path, path_address);
    return status < 0 ? status : vfs::unlink(self()->cwd, path);
}

i64 sys_chdir(u64 path_address) {
    char path[path::kPathMax + 1];
    i64 status = copy_path(path, path_address);
    if (status < 0) return status;
    vfs::Dentry* directory = nullptr;
    if ((status = vfs::open_directory(self()->cwd, path, directory)) < 0) return status;
    sched::Process* process = self();
    vfs::Dentry* old;
    {
        ScopedLock guard(process->files_lock);
        old = process->cwd;
        process->cwd = directory;
    }
    // Another thread may be resolving a path against the old directory right
    // now: then it is kept until the process exits.
    auto* retired = process->threaded ? static_cast<sched::RetiredCwd*>(kmalloc(sizeof(sched::RetiredCwd))) : nullptr;
    if (retired) {
        ScopedLock guard(process->files_lock);
        retired->dentry = old;
        retired->next = process->retired_cwds;
        process->retired_cwds = retired;
    } else if (!process->threaded) {
        vfs::dentry_unref(old);
    }
    return 0;
}

i64 sys_getcwd(u64 buffer, u64 size) {
    char path[path::kPathMax + 1];
    const i64 length = vfs::path_of(self()->cwd, path, sizeof(path));
    if (length < 0) return length;
    if (static_cast<u64>(length) + 1 > size) return error(SIRIUS_EINVAL);
    return usercopy::copy_to(buffer, path, static_cast<u64>(length) + 1) < 0 ? error(SIRIUS_EFAULT) : length;
}

// The address goes into an MSR, and loading a non-canonical one there would
// fault in the kernel: only user-half addresses are accepted.
i64 sys_tcb_set(u64 address) {
    if (address >= kUserEnd) return error(SIRIUS_EINVAL);
    sched::set_thread_pointer(address);
    return 0;
}

i64 sys_clock_get(u64 clock) {
    if (clock != SIRIUS_CLOCK_REALTIME && clock != SIRIUS_CLOCK_MONOTONIC) return error(SIRIUS_EINVAL);
    return static_cast<i64>(clock == SIRIUS_CLOCK_REALTIME ? clock::realtime_ns() : clock::now_ns());
}

i64 sys_sysinfo(u64 address) {
    auto* info = static_cast<sirius_sysinfo*>(kzalloc(sizeof(sirius_sysinfo)));
    if (info == nullptr) return error(SIRIUS_ENOMEM);
    const pmm::Stats memory = pmm::stats();
    info->uptime_ns = clock::now_ns();
    info->memory_total = memory.usable_pages * mm::kPageSize;
    info->memory_free = memory.free_pages * mm::kPageSize;
    info->cpu_count = percpu::count();
    info->process_count = sched::live_processes();
    info->tick_hz = lapic::kTimerHz;
    for (u32 i = 0; i < percpu::count() && i < SIRIUS_MAX_CPUS; i++) {
        const Cpu* cpu = percpu::at(i);
        if (cpu == nullptr) continue;
        info->cpu_ticks[i] = __atomic_load_n(&cpu->ticks, __ATOMIC_RELAXED);
        info->cpu_idle_ticks[i] = __atomic_load_n(&cpu->idle_ticks, __ATOMIC_RELAXED);
    }
    const i64 status = usercopy::copy_to(address, info, sizeof(*info));
    kfree(info);
    return status;
}

i64 sys_proclist(u64 address, u64 capacity) {
    if (capacity > 1024) capacity = 1024;
    if (capacity == 0) return 0;
    auto* list = static_cast<sirius_procinfo*>(kzalloc(capacity * sizeof(sirius_procinfo)));
    if (list == nullptr) return error(SIRIUS_ENOMEM);
    const u64 count = sched::list_processes(list, capacity);
    const i64 status = usercopy::copy_to(address, list, count * sizeof(sirius_procinfo));
    kfree(list);
    return status < 0 ? status : static_cast<i64>(count);
}

} // namespace

void init_cpu() {
    // SYSCALL loads CS from STAR[47:32] and SS from that + 8. SYSRET loads CS
    // from STAR[63:48] + 16 and SS from STAR[63:48] + 8, both with RPL 3 --
    // which is why the GDT keeps user data directly below user code.
    const u64 star = (static_cast<u64>(gdt::kKernelData | 3) << 48) |
                     (static_cast<u64>(gdt::kKernelCode) << 32);
    cpu::wrmsr(cpu::msr::kStar, star);
    cpu::wrmsr(cpu::msr::kLstar, reinterpret_cast<u64>(&syscall_entry));
    cpu::wrmsr(cpu::msr::kSfmask, kEntryFlagsMask);
}

[[noreturn]] void return_to_user(const SyscallFrame& frame) { syscall_return_from(&frame); }

} // namespace syscall

extern "C" void syscall_dispatch(SyscallFrame* frame) {
    using namespace syscall;
    const u64 a = frame->rdi, b = frame->rsi, c = frame->rdx, d = frame->r10, e = frame->r8, f = frame->r9;
    const u64 number = frame->rax;
    i64 result;

    switch (number) {
    case SYS_WRITE:    result = sys_write(a, b, c); break;
    case SYS_READ:     result = sys_read(a, b, c); break;
    case SYS_OPEN:     result = sys_open(a, b, c); break;
    case SYS_CLOSE:    result = sys_close(a); break;
    case SYS_DUP:      result = sys_dup(a); break;
    case SYS_DUP2:     result = sys_dup2(a, b); break;
    case SYS_IOCTL:    result = sys_ioctl(a, b, c); break;
    case SYS_STAT:     result = sys_stat(a, b); break;
    case SYS_FSTAT:    result = sys_fstat(a, b); break;
    case SYS_LSEEK:    result = sys_lseek(a, b, c); break;
    case SYS_READDIR:  result = sys_readdir(a, b); break;
    case SYS_MKDIR:    result = sys_mkdir(a); break;
    case SYS_UNLINK:   result = sys_unlink(a); break;
    case SYS_CHDIR:    result = sys_chdir(a); break;
    case SYS_GETCWD:   result = sys_getcwd(a, b); break;
    case SYS_MMAP:     result = sched::mmap(a, b, c, d, static_cast<i64>(e), f); break;
    case SYS_MUNMAP:   result = sched::munmap(a, b); break;
    case SYS_FORK:     result = sched::fork(*frame); break;
    case SYS_EXECVE:   result = sched::execve(*frame, a, b, c); break;
    case SYS_WAITPID:  result = sched::waitpid(static_cast<i64>(a), b, c); break;
    case SYS_EXIT:     sched::exit_process(static_cast<i32>((a & 0xff) << 8));
    case SYS_YIELD:    sched::yield(); result = 0; break;
    case SYS_SLEEP_MS: result = sched::sleep_interruptible(a); break;
    case SYS_GETCPU:   result = percpu::current()->index; break;
    case SYS_GETPID:   result = self()->pid; break;
    case SYS_GETPPID:  result = sched::parent_pid(self()); break;
    case SYS_TCB_SET:  result = sys_tcb_set(a); break;
    case SYS_CLOCK_GET: result = sys_clock_get(a); break;
    case SYS_SYSINFO:  result = sys_sysinfo(a); break;
    case SYS_PROCLIST: result = sys_proclist(a, b); break;
    case SYS_FCNTL:    result = sys_fcntl(a, b, c); break;
    case SYS_FTRUNCATE: result = sys_ftruncate(a, b); break;
    case SYS_FLOCK:    result = sys_flock(a, b); break;
    case SYS_PREAD:    result = sys_pread(a, b, c, d); break;
    case SYS_PWRITE:   result = sys_pwrite(a, b, c, d); break;
    case SYS_PIPE:     result = ipc::sys_pipe(a, b); break;
    case SYS_POLL:     result = ipc::sys_poll(a, b, static_cast<i64>(c)); break;
    case SYS_KILL:     result = signal::sys_kill(static_cast<i64>(a), b); break;
    case SYS_SIGACTION:   result = signal::sys_sigaction(a, b, c); break;
    case SYS_SIGPROCMASK: result = signal::sys_sigprocmask(a, b, c); break;
    case SYS_SIGPENDING:  result = signal::sys_sigpending(a); break;
    case SYS_SIGSUSPEND:  result = signal::sys_sigsuspend(a); break;
    case SYS_SIGRETURN:   signal::sys_sigreturn(*frame);
    case SYS_SETITIMER:   result = signal::sys_setitimer(a, b, c); break;
    case SYS_SOCKET:      result = ipc::sys_socket(a, b, c); break;
    case SYS_SOCKETPAIR:  result = ipc::sys_socketpair(a, b, c, d); break;
    case SYS_BIND:        result = ipc::sys_bind(a, b, c); break;
    case SYS_LISTEN:      result = ipc::sys_listen(a, b); break;
    case SYS_ACCEPT:      result = ipc::sys_accept(a, b); break;
    case SYS_CONNECT:     result = ipc::sys_connect(a, b, c); break;
    case SYS_SENDMSG:     result = ipc::sys_sendmsg(a, b, c); break;
    case SYS_RECVMSG:     result = ipc::sys_recvmsg(a, b, c); break;
    case SYS_SHUTDOWN:    result = ipc::sys_shutdown(a, b); break;
    case SYS_GETSOCKOPT:  result = ipc::sys_getsockopt(a, b, c, d, e); break;
    case SYS_MEMFD_CREATE: result = ipc::sys_memfd_create(a, b); break;
    case SYS_EPOLL_CREATE: result = ipc::sys_epoll_create(a); break;
    case SYS_EPOLL_CTL:    result = ipc::sys_epoll_ctl(a, b, c, d); break;
    case SYS_EPOLL_WAIT:   result = ipc::sys_epoll_wait(a, b, c, static_cast<i64>(d), e); break;
    case SYS_TIMERFD_CREATE:  result = ipc::sys_timerfd_create(a, b); break;
    case SYS_TIMERFD_SETTIME: result = ipc::sys_timerfd_settime(a, b, c, d); break;
    case SYS_SIGNALFD:     result = ipc::sys_signalfd(static_cast<i64>(a), b, c); break;
    case SYS_EVENTFD:      result = ipc::sys_eventfd(a, b); break;
    case SYS_THREAD_CREATE: result = sched::thread_create(a, b, c); break;
    case SYS_THREAD_EXIT:  sched::exit_thread();
    case SYS_FUTEX_WAIT:   result = sched::futex_wait(a, b, c); break;
    case SYS_FUTEX_WAKE:   result = sched::futex_wake(a, b); break;
    case SYS_GETTID:       result = sched::current()->tid; break;
    case SYS_TKILL:        result = signal::sys_tkill(a, b); break;
    case SYS_SETSID:       result = sched::setsid(); break;
    case SYS_GETSID:       result = a > 0xffffffffULL ? error(SIRIUS_ESRCH) : sched::getsid(static_cast<u32>(a)); break;
    case SYS_SETPGID:      result = a > 0xffffffffULL || b > 0xffffffffULL ? error(SIRIUS_EINVAL)
                                    : sched::setpgid(static_cast<u32>(a), static_cast<u32>(b)); break;
    case SYS_GETPGID:      result = a > 0xffffffffULL ? error(SIRIUS_ESRCH) : sched::getpgid(static_cast<u32>(a)); break;
    case SYS_OPENPTY:      result = ipc::sys_openpty(a, b, c); break;
    default:           result = error(SIRIUS_ENOSYS); break;
    }

    frame->rax = static_cast<u64>(result);
    // Files the call looked up were kept open for it until here.
    sched::release_pins();
    // If the call woke a thread on this CPU, that thread goes first.
    sched::preempt_point();
    // The last thing before going back: act on any signal that is due. This
    // may enter a handler, restart the call, or not return at all.
    signal::on_syscall_return(*frame, number);
}
