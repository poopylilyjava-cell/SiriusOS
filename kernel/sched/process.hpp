#pragma once
#include <sched/thread.hpp>
#include <sched/waitqueue.hpp>
#include <arch/x86_64/syscall.hpp>
#include <mm/vmm.hpp>
#include <sirius/signal.h>

struct sirius_procinfo;
struct Registers;

namespace vfs { struct File; struct Dentry; }

namespace sched {

constexpr u32 kMaxFiles = 256;

enum class ProcessState : u8 { Running, Zombie };

// A working directory a threaded process has left. Another thread may be in
// the middle of resolving a path against it, so it is kept until the process
// exits rather than dropped at once.
struct RetiredCwd {
    vfs::Dentry* dentry;
    RetiredCwd* next;
};

// A user program: an address space, open files, a place in the process tree
// and one or more threads.
//
// All of a process's threads run on one CPU (the one its first thread was
// placed on). Two of them are therefore never in the kernel at the same
// instant, but one can be preempted or go to sleep in the middle of a system
// call while another runs: what they share is guarded accordingly.
struct Process {
    u32  pid;
    char name[32];
    // Changed only on the process's CPU, an operation at a time with
    // interrupts off there (vmm does that), which is also how a page fault
    // sees it.
    vmm::AddressSpace space;

    Spinlock threads_lock;   // the four fields below; taken inside the tree lock, never around it
    Thread* threads;
    u32  thread_count;
    // Set while one thread is ending the others, to exit or to exec: they
    // leave at their next return to user mode, and it waits for them.
    Thread* exit_leader;
    u64  exited_ticks;       // CPU time of threads that are gone
    // Has had more than one thread since it was last exec'd. A process that
    // has not owns its descriptor table outright.
    bool threaded;

    // The tree, guarded by the process tree lock in process.cpp.
    Process* parent;     // null once orphaned
    Process* children;
    Process* sibling;
    ProcessState state;
    i32 wait_status;     // Linux encoding (sirius/fs.h); valid once Zombie

    Process* all_next;   // every live process, for lookup by pid; under the tree lock

    // Sessions and process groups, as POSIX has them -- for terminals: a
    // pseudo-terminal sends Ctrl+C's SIGINT to its foreground group. Both
    // inherited across fork; a process started by the kernel leads its own.
    u32 sid;
    u32 pgid;

    // The descriptor table, under files_lock once the process is threaded.
    // Looking a descriptor up goes through file_of().
    Spinlock     files_lock;
    vfs::File*   files[kMaxFiles];
    u8           fd_flags[kMaxFiles];   // per descriptor, not per open file: SIRIUS_FD_CLOEXEC
    vfs::Dentry* cwd;
    RetiredCwd*  retired_cwds;
    u64 mmap_next;       // where the next non-fixed mmap goes; advanced atomically

    SyscallFrame fork_frame;   // the child's first return to ring 3

    // Signals (sched/signal.cpp). Pending is a bit set, signal n at bit
    // n - 1: the signals sent to the process, which any of its threads that
    // does not block them may take. Other processes and interrupt handlers
    // set bits, so it is accessed atomically. Each thread has its own mask.
    u64 sig_pending;
    sirius_sigaction sig_actions[SIRIUS_NSIG];
    u64 alarm_deadline_ns;     // setitimer(ITIMER_REAL): 0 when disarmed
    u64 alarm_interval_ns;
};

// The lowest free descriptor at or above `minimum` now refers to `file`
// (whose reference the table takes over). -EMFILE if there is none.
i64 install_fd(Process* process, vfs::File* file, u32 fd_flags = 0, u32 minimum = 0);

// The open file behind a descriptor of the calling process, or null. It stays
// valid until the system call returns, even if another thread closes the
// descriptor meanwhile: in a threaded process the calling thread holds a
// reference until then.
vfs::File* file_of(u64 fd);
// Drop the references file_of() took. At the end of every system call.
void release_pins();

// Take a descriptor out of the table: the file it referred to, whose
// reference is now the caller's to close, or null.
vfs::File* detach_fd(Process* process, u64 fd);
// Make `fd` refer to `file` (taking over that reference), whatever it
// referred to before -- which is returned, for the caller to close.
vfs::File* replace_fd(Process* process, u32 fd, vfs::File* file, u32 fd_flags);

// Load a static ELF executable into a new process with `argv` and start it.
// It gets /dev/console as stdin, stdout and stderr, and "/" as its cwd. Null,
// after logging why, if the image is invalid or memory runs out.
// `path` is what the program is told its executable is (AT_EXECFN); `name` if null.
Process* spawn_process(const char* name, const void* image, usize size,
                       const char* const* argv, u32 argc, const char* path = nullptr);

// The same, reading the executable from the filesystem.
Process* spawn_process_from_path(const char* path, const char* const* argv, u32 argc);

// --- system call implementations (current process) ---------------------------------

i64 fork(const SyscallFrame& frame);
// Only returns on failure. `frame` is rewritten to enter the new program.
i64 execve(SyscallFrame& frame, u64 path, u64 argv, u64 envp);
i64 waitpid(i64 pid, u64 status_address, u64 options);
[[noreturn]] void exit_process(i32 wait_status);

// Threads. A new one starts in ring 3 at `entry` on `stack`, with `tcb` as
// its thread pointer; the call returns its id. It shares everything with its
// creator but the signal mask, which it gets a copy of.
i64 thread_create(u64 entry, u64 stack, u64 tcb);
// End the calling thread. The process ends with its last one, with status 0.
[[noreturn]] void exit_thread();
// Is another thread of this one's process ending the process (or replacing
// it, with exec)? Then this one must go, at its next return to user mode.
bool dying(const Thread* thread);
// Send `sig` to one thread of the calling process. -ESRCH if it has none
// with that id.
i64 signal_thread(u32 tid, int sig);

// A fault in the kernel, on a user address, in a threaded process: another
// thread has unmapped what this one's system call was reading or writing.
// The call is given a page of zeroes to finish with and the process is
// killed. False if this is not such a fault.
bool absorb_kernel_fault(const Registers* regs);

// futex (sched/futex.cpp): sleep while the word at `address` holds
// `expected`, until woken or `timeout_ns` (~0 for none) has passed.
i64 futex_wait(u64 address, u64 expected, u64 timeout_ns);
i64 futex_wake(u64 address, u64 count);
i64 mmap(u64 address, u64 length, u64 protection, u64 flags, i64 fd, u64 offset);
i64 munmap(u64 address, u64 length);

// Set the calling thread's FS base -- where user code finds its thread
// control block and thread-local storage. `address` must be canonical.
void set_thread_pointer(u64 address);

// Called by the scheduler when the process's last thread has been reaped.
void release_process(Process* process);

// Processes started and not yet released.
u32 live_processes();

Process* current_process();

// The parent's pid, or 0 if orphaned. Under the tree lock: the parent may be
// exiting and being freed concurrently.
u32 parent_pid(Process* process);

// Post `sig` to the process with this pid (0 just checks that it exists).
// -ESRCH if there is none.
i64 signal_process(u32 pid, int sig);
// The same, to every process in a group. -ESRCH if the group is empty.
i64 signal_group(u32 pgid, int sig);

// Sessions and groups (system calls, for the current process).
i64 setsid();
i64 getsid(u32 pid);
i64 setpgid(u32 pid, u32 pgid);
i64 getpgid(u32 pid);

// Describe up to `capacity` processes, live and zombie. Returns how many.
u64 list_processes(sirius_procinfo* out, u64 capacity);

// Post SIGALRM to every process whose interval timer has reached `now_ns`.
void expire_alarms(u64 now_ns);

// Sleep for at least this long, or until a signal arrives: 0 or -EINTR.
i64 sleep_interruptible(u64 milliseconds);

} // namespace sched
