#include <sched/process.hpp>
#include <sched/sched.hpp>
#include <sched/events.hpp>
#include <sched/signal.hpp>
#include <arch/x86_64/clock.hpp>
#include <exec/elf.hpp>
#include <fs/vfs.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <mm/heap.hpp>
#include <mm/paging_math.hpp>
#include <mm/pmm.hpp>
#include <mm/usercopy.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/syscall.h>
#include <sirius/sysinfo.h>

extern "C" [[noreturn]] void syscall_return_from(const SyscallFrame* frame);

namespace sched {
namespace {

// The stack sits just below the top of the lower half. The last canonical
// page above it is left unmapped, and so are the pages below the reservation,
// so running off either end faults.
constexpr u64 kUserStackTop   = 0x00007ffffffff000ULL;
constexpr u64 kUserStackPages = 64;                     // 256 KiB, demand paged
constexpr u64 kMmapBase       = 0x0000100000000000ULL;  // 16 TiB, far from the image

constexpr u32 kMaxArguments      = 64;     // argv and envp entries, each
constexpr u64 kMaxArgumentBytes  = 16384;  // all their strings together
constexpr u64 kMaxExecutableSize = 4 * 1024 * 1024;

// Auxiliary vector types (System V ABI). A C library reads these at startup
// to find its own program headers -- and with them its TLS template.
constexpr u64 kAtNull   = 0;
constexpr u64 kAtPhdr   = 3;
constexpr u64 kAtPhent  = 4;
constexpr u64 kAtPhnum  = 5;
constexpr u64 kAtPagesz = 6;
constexpr u64 kAtEntry  = 9;
constexpr u64 kAtExecfn = 31;
constexpr u64 kAuxEntries = 7;   // the six above, and AT_NULL

constexpr u64 kInitialUserFlags = 0x202;  // IF set, plus the always-one bit 1

Spinlock g_tree_lock;   // parent, children, sibling, state, wait_status, and the list below
Process* g_all = nullptr;
u32 g_next_pid = 1;
u32 g_live = 0;

// argv and envp, gathered into kernel memory: execve must have them before
// the address space they came from is torn down.
struct Arguments {
    u32 argc, envc;
    const char* argv[kMaxArguments];
    const char* envp[kMaxArguments];
    const char* path;      // the executable, for AT_EXECFN
    u64 used;
    char strings[kMaxArgumentBytes];

    bool set_path(const char* string) {
        const u64 length = strlen(string) + 1;
        if (length > kMaxArgumentBytes - used) return false;
        memcpy(strings + used, string, length);
        path = strings + used;
        used += length;
        return true;
    }

    bool add(const char* string, bool environment) {
        const u64 length = strlen(string) + 1;
        u32& count = environment ? envc : argc;
        if (count == kMaxArguments || length > kMaxArgumentBytes - used) return false;
        memcpy(strings + used, string, length);
        (environment ? envp : argv)[count++] = strings + used;
        used += length;
        return true;
    }
};

const char* basename(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; p++) if (*p == '/' && p[1]) name = p + 1;
    return name;
}

u64 user_page_flags(bool writable, bool executable) {
    u64 page_flags = vmm::flags::kPresent | vmm::flags::kUser;
    if (writable)    page_flags |= vmm::flags::kWrite;
    if (!executable) page_flags |= vmm::flags::kNoExecute;
    return page_flags;
}

// Copy one segment into freshly allocated pages. The space is not loaded in
// any CPU's CR3 yet, so everything goes through the direct map and no TLB
// can be stale.
bool load_segment(vmm::AddressSpace& space, const u8* file, const elf::Segment& segment) {
    const u64 first = mm::align_down(segment.virtual_address, mm::kPageSize);
    const u64 end   = mm::align_up(segment.virtual_address + segment.memory_size, mm::kPageSize);
    const u64 file_end = segment.virtual_address + segment.file_size;

    for (u64 page = first; page < end; page += mm::kPageSize) {
        u64* entry = space.entry_for(page, true, vmm::flags::kUser);
        if (entry == nullptr) return false;

        u64 physical;
        if (*entry & vmm::flags::kPresent) {
            // Two segments share this page (parse() guarantees they share no
            // bytes). It gets the union of their permissions.
            physical = *entry & vmm::flags::kAddressMask;
            if (segment.writable)   *entry |= vmm::flags::kWrite;
            if (segment.executable) *entry &= ~vmm::flags::kNoExecute;
        } else {
            physical = pmm::alloc_page_zeroed();
            if (physical == 0) return false;
            *entry = physical | user_page_flags(segment.writable, segment.executable);
        }

        // The slice of file contents that lands in this page; the rest stays
        // zero, which is what a .bss tail needs.
        const u64 copy_start = page > segment.virtual_address ? page : segment.virtual_address;
        const u64 copy_end   = (page + mm::kPageSize) < file_end ? page + mm::kPageSize : file_end;
        if (copy_start < copy_end) {
            memcpy(mm::phys_to_virt<u8>(physical) + (copy_start - page),
                   file + segment.file_offset + (copy_start - segment.virtual_address),
                   copy_end - copy_start);
        }
    }
    return true;
}

int load_image(vmm::AddressSpace& space, const void* image, usize size, elf::Image& layout, const char* name) {
    const elf::Error error = elf::parse(image, size, layout);
    if (error != elf::Error::None) {
        LOG_WARN("exec '%s': rejected: %s", name, elf::error_name(error));
        return -SIRIUS_ENOEXEC;
    }
    for (u32 i = 0; i < layout.segment_count; i++)
        if (!load_segment(space, static_cast<const u8*>(image), layout.segments[i])) return -SIRIUS_ENOMEM;
    return 0;
}

// Write into a space that is not loaded, through the direct map. Every page
// touched must already be mapped.
void poke(vmm::AddressSpace& space, u64 address, const void* data, u64 length) {
    const auto* in = static_cast<const u8*>(data);
    while (length) {
        const u64 physical = space.translate(address);
        const u64 n = mm::kPageSize - (address % mm::kPageSize) < length
                    ? mm::kPageSize - (address % mm::kPageSize) : length;
        memcpy(mm::phys_to_virt<u8>(physical), in, n);
        address += n; in += n; length -= n;
    }
}

// The System V process entry stack, top down: the strings, then (16-byte
// aligned) argc, argv[], NULL, envp[], NULL, and the auxiliary vector.
// Pages holding all of that are mapped now; the rest of the stack faults in.
bool build_stack(vmm::AddressSpace& space, const Arguments& args, const elf::Image& image, u64& stack_pointer) {
    const u64 pointer_bytes = 8 * (1 + args.argc + 1 + args.envc + 1 + 2 * kAuxEntries);
    const u64 strings_start = kUserStackTop - mm::align_up(args.used, 16);
    const u64 frame = mm::align_down(strings_start - pointer_bytes, 16);
    const u64 first_page = mm::align_down(frame, mm::kPageSize);
    if ((kUserStackTop - first_page) / mm::kPageSize > kUserStackPages) return false;

    const u64 stack_flags = user_page_flags(true, false);
    for (u64 page = kUserStackTop - kUserStackPages * mm::kPageSize; page < kUserStackTop; page += mm::kPageSize) {
        if (page < first_page) {
            if (!space.map_demand(page, stack_flags)) return false;
            continue;
        }
        const u64 physical = pmm::alloc_page_zeroed();
        if (physical == 0) return false;
        if (!space.map(page, physical, stack_flags)) { pmm::free_page(physical); return false; }
    }

    poke(space, strings_start, args.strings, args.used);

    // Rebase each string pointer from the kernel buffer to the user copy.
    auto user_address = [&](const char* s) { return strings_start + static_cast<u64>(s - args.strings); };
    u64 cursor = frame;
    auto push = [&](u64 value) { poke(space, cursor, &value, 8); cursor += 8; };
    push(args.argc);
    for (u32 i = 0; i < args.argc; i++) push(user_address(args.argv[i]));
    push(0);
    for (u32 i = 0; i < args.envc; i++) push(user_address(args.envp[i]));
    push(0);
    auto aux = [&](u64 type, u64 value) { push(type); push(value); };
    aux(kAtPhdr,   image.program_headers);
    aux(kAtPhent,  elf::kProgramHeaderSize);
    aux(kAtPhnum,  image.program_header_count);
    aux(kAtPagesz, mm::kPageSize);
    aux(kAtEntry,  image.entry);
    aux(kAtExecfn, args.path ? user_address(args.path) : 0);
    aux(kAtNull,   0);

    stack_pointer = frame;
    return true;
}

SyscallFrame entry_frame(u64 entry, u64 stack_pointer) {
    SyscallFrame frame{};
    frame.rip    = entry;
    frame.rsp    = stack_pointer;
    frame.rflags = kInitialUserFlags;
    frame.cs     = gdt::kUserCodeSelector;
    frame.ss     = gdt::kUserDataSelector;
    return frame;
}

// Every user thread starts here, on its own kernel stack, and leaves for
// ring 3 through the system call exit path with its prepared frame.
[[noreturn]] void start_from_frame(void* arg) {
    auto* process = static_cast<Process*>(arg);
    LOG_DEBUG("process %u '%s': entering ring 3 at %p on cpu %u", process->pid, process->name,
              reinterpret_cast<void*>(process->fork_frame.rip), percpu::current()->index);
    syscall_return_from(&process->fork_frame);
}

// The same for a thread made by thread_create, whose frame is its own.
[[noreturn]] void start_thread(void* arg) {
    const SyscallFrame frame = *static_cast<SyscallFrame*>(arg);
    kfree(arg);
    // The process may have been ended before this thread ever ran.
    if (dying(current())) exit_thread();
    syscall_return_from(&frame);
}

// `thread` is one of the process's from here on.
void attach(Process* process, Thread* thread) {
    ScopedLock guard(process->threads_lock);
    thread->process_next = process->threads;
    process->threads = thread;
    process->thread_count++;
}

// The calling thread ends the others, and returns when they are gone. False
// if another thread got there first: then this one is among those to go.
bool end_other_threads(Process* process, i32 wait_status) {
    Thread* self = current();
    {
        ScopedLock guard(process->threads_lock);
        if (process->exit_leader && process->exit_leader != self) return false;
        process->exit_leader = self;
        process->wait_status = wait_status;
    }
    // They share this CPU, so none is running now. Each leaves when it next
    // heads back to user mode; one asleep in a system call is woken to do so.
    events::notify();
    while (__atomic_load_n(&process->thread_count, __ATOMIC_ACQUIRE) > 1) yield();
    return true;
}

void close_all(Process* process) {
    // What chdir set aside while other threads might still have been using it.
    while (RetiredCwd* retired = process->retired_cwds) {
        process->retired_cwds = retired->next;
        vfs::dentry_unref(retired->dentry);
        kfree(retired);
    }
    for (auto& file : process->files) {
        // Out of the table first: closing may block, and may look at it.
        vfs::File* closing = file;
        file = nullptr;
        vfs::file_close(closing);
    }
    vfs::dentry_unref(process->cwd);
    process->cwd = nullptr;
}

Process* new_process(const char* name) {
    auto* process = static_cast<Process*>(kzalloc(sizeof(Process)));
    if (process == nullptr) return nullptr;
    process->pid = __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_RELAXED);
    process->sid = process->pgid = process->pid;
    str_copy(process->name, sizeof(process->name), name);
    process->mmap_next = kMmapBase;
    if (!process->space.create()) { kfree(process); return nullptr; }
    return process;
}

void discard(Process* process) {
    close_all(process);
    process->space.destroy();
    kfree(process);
}

// Start a new process's first thread.
void launch(Process* process, Thread* thread) {
    __atomic_fetch_add(&g_live, 1, __ATOMIC_RELAXED);
    attach(process, thread);
    {
        ScopedLock guard(g_tree_lock);
        process->all_next = g_all;
        g_all = process;
    }
    start(thread);
}

// Out of the pid lookup list. Caller holds the tree lock.
void forget(Process* process) {
    for (Process** link = &g_all; *link; link = &(*link)->all_next)
        if (*link == process) { *link = process->all_next; return; }
}

i32 signal_for_vector(u64 vector) {
    switch (vector) {
    case 0: case 16: case 19: return 8;    // SIGFPE
    case 6:                   return 4;    // SIGILL
    case 1: case 3:           return 5;    // SIGTRAP
    case 17:                  return 7;    // SIGBUS
    default:                  return 11;   // SIGSEGV
    }
}

} // namespace

Process* current_process() { return current()->process; }

Process* spawn_process(const char* name, const void* image, usize size, const char* const* argv, u32 argc,
                       const char* path) {
    auto* args = static_cast<Arguments*>(kzalloc(sizeof(Arguments)));
    Process* process = args ? new_process(name) : nullptr;
    if (process == nullptr) { kfree(args); LOG_ERROR("process '%s': out of memory", name); return nullptr; }

    bool ok = true;
    for (u32 i = 0; i < argc && ok; i++) ok = args->add(argv[i], false);
    ok = ok && args->set_path(path ? path : name);
    if (!ok) LOG_ERROR("process '%s': arguments too large", name);

    elf::Image layout;
    u64 stack = 0;
    ok = ok && load_image(process->space, image, size, layout, name) == 0;
    ok = ok && build_stack(process->space, *args, layout, stack);
    kfree(args);

    // Standard streams on the console, and "/" to start from.
    for (u32 fd = 0; ok && fd < 3; fd++) {
        vfs::File* file = nullptr;
        if (vfs::open(nullptr, "/dev/console", SIRIUS_O_RDWR, file) == 0) process->files[fd] = file;
    }
    if (ok && vfs::root()) {
        process->cwd = vfs::root();
        vfs::dentry_ref(process->cwd);
    }

    Thread* thread = nullptr;
    if (ok) {
        process->fork_frame = entry_frame(layout.entry, stack);
        thread = create(name, start_from_frame, process, process);
        ok = thread != nullptr;
    }
    if (!ok) {
        LOG_ERROR("process '%s': could not be started", name);
        discard(process);
        return nullptr;
    }

    launch(process, thread);
    return process;
}

Process* spawn_process_from_path(const char* path, const char* const* argv, u32 argc) {
    void* image = nullptr;
    u64 size = 0;
    const int status = vfs::read_whole_file(nullptr, path, image, size, kMaxExecutableSize);
    if (status < 0) {
        LOG_ERROR("process '%s': cannot read the executable (error %d)", path, -status);
        return nullptr;
    }
    Process* process = spawn_process(basename(path), image, size, argv, argc, path);
    kfree(image);
    return process;
}

i64 fork(const SyscallFrame& frame) {
    Process* parent = current_process();
    Process* child = new_process(parent->name);
    if (child == nullptr) return -SIRIUS_ENOMEM;

    // Only the calling thread is copied: the child has one thread, whatever
    // the parent has.
    if (!parent->space.fork_into(child->space)) { discard(child); return -SIRIUS_ENOMEM; }
    {
        ScopedLock guard(parent->files_lock);
        for (u32 fd = 0; fd < kMaxFiles; fd++) {
            if (parent->files[fd]) child->files[fd] = vfs::file_ref(parent->files[fd]);
            child->fd_flags[fd] = parent->fd_flags[fd];
        }
        child->cwd = parent->cwd;
        if (child->cwd) vfs::dentry_ref(child->cwd);
    }
    signal::on_fork(parent, child);
    child->sid = parent->sid;
    child->pgid = parent->pgid;
    child->mmap_next = __atomic_load_n(&parent->mmap_next, __ATOMIC_RELAXED);

    // The child resumes at the same instruction, seeing fork() return 0.
    child->fork_frame = frame;
    child->fork_frame.rax = 0;

    Thread* thread = create(child->name, start_from_frame, child, child);
    if (thread == nullptr) { discard(child); return -SIRIUS_ENOMEM; }
    // The caller's FPU registers are live on this CPU: the kernel itself never
    // touches them, so they are exactly its user state.
    cpu::fxsave(thread->fpu_state);
    thread->fs_base = current()->fs_base;
    thread->sig_blocked = current()->sig_blocked;

    {
        ScopedLock guard(g_tree_lock);
        child->parent = parent;
        child->sibling = parent->children;
        parent->children = child;
    }

    const u32 pid = child->pid;
    launch(child, thread);   // `child` may run, exit and be freed from here on
    return pid;
}

i64 execve(SyscallFrame& frame, u64 path_address, u64 argv_address, u64 envp_address) {
    Process* process = current_process();

    char path[path::kPathMax + 1];
    const i64 length = usercopy::string_from(path, path_address, sizeof(path));
    if (length < 0) return length;

    auto* args = static_cast<Arguments*>(kzalloc(sizeof(Arguments)));
    if (args == nullptr) return -SIRIUS_ENOMEM;

    // Gather argv, then envp: arrays of user pointers ending in null.
    i64 status = 0;
    for (int list = 0; list < 2 && status == 0; list++) {
        const u64 base = list == 0 ? argv_address : envp_address;
        if (base == 0) continue;
        for (u32 i = 0;; i++) {
            u64 pointer;
            if (i == kMaxArguments) { status = -SIRIUS_E2BIG; break; }
            if ((status = usercopy::copy_from(&pointer, base + i * 8, 8)) < 0) break;
            if (pointer == 0) break;
            char string[1024];
            const i64 n = usercopy::string_from(string, pointer, sizeof(string));
            if (n < 0) { status = n == -SIRIUS_ENAMETOOLONG ? -SIRIUS_E2BIG : n; break; }
            if (!args->add(string, list == 1)) { status = -SIRIUS_E2BIG; break; }
        }
    }

    if (status == 0 && !args->set_path(path)) status = -SIRIUS_E2BIG;

    void* image = nullptr;
    u64 size = 0;
    if (status == 0) status = vfs::read_whole_file(process->cwd, path, image, size, kMaxExecutableSize);

    // Build the whole new image aside: until it is complete, failing still
    // returns to the old program intact.
    vmm::AddressSpace fresh;
    elf::Image layout;
    u64 stack = 0;
    if (status == 0 && !fresh.create()) status = -SIRIUS_ENOMEM;
    if (status == 0) status = load_image(fresh, image, size, layout, path);
    if (status == 0 && !build_stack(fresh, *args, layout, stack)) status = -SIRIUS_ENOMEM;
    kfree(image);
    kfree(args);
    if (status < 0) {
        fresh.destroy();
        return status;
    }

    // The new program starts with one thread: this one. The others go first,
    // while there is still an old program for them to be leaving.
    if (process->threaded) {
        if (!end_other_threads(process, 0)) exit_thread();
        ScopedLock guard(process->threads_lock);
        process->exit_leader = nullptr;
        process->threaded = false;
    }

    // The point of no return.
    vmm::AddressSpace old = process->space;
    process->space = fresh;
    process->space.activate();
    old.destroy();

    // Descriptors marked close-on-exec stop here; the rest are the new
    // program's inheritance.
    for (u32 fd = 0; fd < kMaxFiles; fd++) {
        if (!(process->fd_flags[fd] & SIRIUS_FD_CLOEXEC)) continue;
        vfs::File* closing = process->files[fd];
        process->files[fd] = nullptr;
        process->fd_flags[fd] = 0;
        vfs::file_close(closing);
    }
    signal::on_exec(process);

    str_copy(process->name, sizeof(process->name), basename(path));
    process->mmap_next = kMmapBase;
    cpu::fxrstor(cpu::initial_fpu_state());
    set_thread_pointer(0);
    frame = entry_frame(layout.entry, stack);
    LOG_DEBUG("process %u: exec '%s'", process->pid, path);
    return 0;
}

i64 waitpid(i64 pid, u64 status_address, u64 options) {
    if (status_address && !usercopy::writable(status_address, sizeof(i32))) return -SIRIUS_EFAULT;
    if (options & ~static_cast<u64>(SIRIUS_WNOHANG)) return -SIRIUS_EINVAL;
    Process* self = current_process();

    for (;;) {
        // A child that exits after this snapshot notifies, and the wait
        // below then returns at once.
        const u64 snapshot = events::snapshot();
        bool any = false;
        Process* zombie = nullptr;
        {
            ScopedLock guard(g_tree_lock);
            for (Process** link = &self->children; *link; link = &(*link)->sibling) {
                Process* child = *link;
                if (pid > 0 && child->pid != static_cast<u64>(pid)) continue;
                any = true;
                if (child->state == ProcessState::Zombie) {
                    *link = child->sibling;
                    zombie = child;
                    break;
                }
            }
        }

        if (zombie) {
            const i32 status = zombie->wait_status;
            const u32 child_pid = zombie->pid;
            kfree(zombie);
            if (status_address) usercopy::copy_to(status_address, &status, sizeof(status));
            return child_pid;
        }
        if (!any) return -SIRIUS_ECHILD;
        if (options & SIRIUS_WNOHANG) return 0;
        if (events::wait(snapshot) == -SIRIUS_EINTR) return signal::kRestart;
    }
}

[[noreturn]] void exit_process(i32 wait_status) {
    Process* process = current_process();
    if (process == nullptr) panic("exit_process from kernel thread %u", current()->tid);

    release_pins();
    // With several threads, the first to get here ends the process and the
    // rest just end.
    if (!end_other_threads(process, wait_status)) exit_thread();

    close_all(process);

    {
        ScopedLock guard(g_tree_lock);
        // Children nobody will wait for: a zombie goes now, a live one frees
        // itself when it exits.
        for (Process* child = process->children; child;) {
            Process* next = child->sibling;
            if (child->state == ProcessState::Zombie) kfree(child);
            else child->parent = nullptr;
            child = next;
        }
        process->children = nullptr;
        // No more signals: nothing will ever act on them, and the pid is
        // about to stop meaning this process.
        forget(process);
    }
    exit_current();
}

void release_process(Process* process) {
    // The thread has switched away for good, and with it the CPU's CR3.
    process->space.destroy();

    const i32 status = process->wait_status;
    if (SIRIUS_WIFEXITED(status))
        LOG_INFO("process %u '%s': exited with status %d", process->pid, process->name, SIRIUS_WEXITSTATUS(status));
    else
        LOG_INFO("process %u '%s': killed by signal %d", process->pid, process->name, SIRIUS_WTERMSIG(status));

    bool orphan;
    {
        ScopedLock guard(g_tree_lock);
        orphan = process->parent == nullptr;
        {
            ScopedLock threads(process->threads_lock);
            process->threads = nullptr;
            process->thread_count = 0;
        }
        if (!orphan) {
            process->state = ProcessState::Zombie;
            signal::post(process->parent, SIRIUS_SIGCHLD);
        }
    }
    if (orphan) kfree(process);
    // waitpid sleeps on the event channel, and so does anything polling for
    // what the exit just closed.
    events::notify();

    if (__atomic_sub_fetch(&g_live, 1, __ATOMIC_ACQ_REL) == 0)
        kprintf("sirius: all user processes exited\n");
}

void set_thread_pointer(u64 address) {
    current()->fs_base = address;
    cpu::wrmsr(cpu::msr::kFsBase, address);
}

i64 thread_create(u64 entry, u64 stack, u64 tcb) {
    constexpr u64 kUserEnd = 0x0000800000000000ULL;
    // The thread pointer goes into an MSR, and the entry point into a return
    // to ring 3: neither may be an address only the kernel could use.
    if (entry >= kUserEnd || tcb >= kUserEnd || stack >= kUserEnd) return -SIRIUS_EINVAL;
    Process* process = current_process();
    Thread* self = current();

    auto* frame = static_cast<SyscallFrame*>(kmalloc(sizeof(SyscallFrame)));
    if (frame == nullptr) return -SIRIUS_ENOMEM;
    *frame = entry_frame(entry, stack);

    // On the creator's CPU, like every thread of the process: its address
    // space is then only ever loaded there, and needs no TLB shootdowns.
    Thread* thread = create(process->name, start_thread, frame, process, self->cpu);
    if (thread == nullptr) { kfree(frame); return -SIRIUS_EAGAIN; }
    thread->fs_base = tcb;
    thread->sig_blocked = self->sig_blocked;

    const u32 tid = thread->tid;
    process->threaded = true;
    attach(process, thread);
    start(thread);
    return tid;
}

bool dying(const Thread* thread) {
    const Process* process = thread->process;
    if (process == nullptr) return false;
    const Thread* leader = __atomic_load_n(&process->exit_leader, __ATOMIC_ACQUIRE);
    return leader != nullptr && leader != thread;
}

[[noreturn]] void exit_thread() {
    Thread* self = current();
    Process* process = self->process;
    if (process == nullptr) panic("exit_thread from kernel thread %u", self->tid);
    release_pins();

    // The last thread takes the process with it.
    if (!dying(self) && __atomic_load_n(&process->thread_count, __ATOMIC_ACQUIRE) == 1) exit_process(0);

    // From here to the switch away nothing may interrupt: once the count has
    // dropped, a thread ending the process stops waiting for this one, and
    // the process can be gone the next time this CPU looks.
    irq::save();
    process->threads_lock.lock();
    for (Thread** link = &process->threads; *link; link = &(*link)->process_next)
        if (*link == self) { *link = self->process_next; break; }
    process->exited_ticks += __atomic_load_n(&self->run_ticks, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&process->thread_count, 1, __ATOMIC_ACQ_REL);
    process->threads_lock.unlock();
    self->process = nullptr;   // nothing of it is this thread's to release
    events::notify();
    exit_current();
}

i64 signal_thread(u32 tid, int sig) {
    Process* process = current_process();
    ScopedLock guard(process->threads_lock);
    for (Thread* thread = process->threads; thread; thread = thread->process_next) {
        if (thread->tid != tid) continue;
        if (sig) signal::post(thread, sig);
        return 0;
    }
    return -SIRIUS_ESRCH;
}

bool absorb_kernel_fault(const Registers* regs) {
    Process* process = current_process();
    const u64 address = cpu::read_cr2();
    if (process == nullptr || !process->threaded || regs->vector != 14) return false;
    if (address < mm::kPageSize || address >= 0x0000800000000000ULL) return false;

    const u64 page = mm::align_down(address, mm::kPageSize);
    const u64 physical = pmm::alloc_page_zeroed();
    if (physical == 0) return false;
    process->space.release(page);
    if (!process->space.map(page, physical, user_page_flags(true, false))) { pmm::free_page(physical); return false; }

    LOG_ERROR("process %u '%s': a thread's system call (rip=%p) lost its memory at %p to another thread; killing it",
              process->pid, process->name, reinterpret_cast<void*>(regs->rip), reinterpret_cast<void*>(address));
    signal::post(process, SIRIUS_SIGKILL);
    return true;
}

vfs::File* file_of(u64 fd) {
    if (fd >= kMaxFiles) return nullptr;
    Thread* self = current();
    Process* process = self->process;
    // The only thread there has ever been: nobody else can close it.
    if (!process->threaded) return process->files[fd];

    // Room for one more reference, found before the lock is taken.
    if (self->pin_count == self->pin_capacity) {
        const u32 capacity = self->pin_capacity ? self->pin_capacity * 2 : 8;
        auto** pins = static_cast<vfs::File**>(kmalloc(capacity * sizeof(vfs::File*)));
        if (pins == nullptr) return nullptr;
        if (self->pin_count) memcpy(pins, self->pins, self->pin_count * sizeof(vfs::File*));
        kfree(self->pins);
        self->pins = pins;
        self->pin_capacity = capacity;
    }

    ScopedLock guard(process->files_lock);
    vfs::File* file = process->files[fd];
    if (file == nullptr) return nullptr;
    // poll looks its descriptors up again every time it wakes: one
    // reference per file is enough.
    for (u32 i = 0; i < self->pin_count; i++) if (self->pins[i] == file) return file;
    self->pins[self->pin_count++] = vfs::file_ref(file);
    return file;
}

void release_pins() {
    Thread* self = current();
    while (self->pin_count) vfs::file_close(self->pins[--self->pin_count]);
}

vfs::File* detach_fd(Process* process, u64 fd) {
    if (fd >= kMaxFiles) return nullptr;
    ScopedLock guard(process->files_lock);
    vfs::File* file = process->files[fd];
    process->files[fd] = nullptr;
    process->fd_flags[fd] = 0;
    return file;
}

vfs::File* replace_fd(Process* process, u32 fd, vfs::File* file, u32 fd_flags) {
    ScopedLock guard(process->files_lock);
    vfs::File* replaced = process->files[fd];
    process->files[fd] = file;
    process->fd_flags[fd] = static_cast<u8>(fd_flags);
    return replaced;
}

i64 install_fd(Process* process, vfs::File* file, u32 fd_flags, u32 minimum) {
    ScopedLock guard(process->files_lock);
    for (u32 fd = minimum; fd < kMaxFiles; fd++) {
        if (process->files[fd]) continue;
        process->files[fd] = file;
        process->fd_flags[fd] = static_cast<u8>(fd_flags);
        return fd;
    }
    return -SIRIUS_EMFILE;
}

i64 signal_process(u32 pid, int sig) {
    ScopedLock guard(g_tree_lock);
    for (Process* process = g_all; process; process = process->all_next) {
        if (process->pid != pid) continue;
        if (sig) signal::post(process, sig);
        return 0;
    }
    return -SIRIUS_ESRCH;
}

i64 signal_group(u32 pgid, int sig) {
    ScopedLock guard(g_tree_lock);
    bool found = false;
    for (Process* process = g_all; process; process = process->all_next) {
        if (process->pgid != pgid || process->state == ProcessState::Zombie) continue;
        if (sig) signal::post(process, sig);
        found = true;
    }
    return found ? 0 : -SIRIUS_ESRCH;
}

namespace {

// The live process with this pid, under the tree lock; 0 means the caller.
Process* find_locked(u32 pid) {
    if (pid == 0) return current_process();
    for (Process* process = g_all; process; process = process->all_next)
        if (process->pid == pid && process->state != ProcessState::Zombie) return process;
    return nullptr;
}

} // namespace

i64 setsid() {
    Process* process = current_process();
    ScopedLock guard(g_tree_lock);
    // A group leader may not leave its group behind in another session.
    if (process->pgid == process->pid) return -SIRIUS_EPERM;
    process->sid = process->pgid = process->pid;
    return process->sid;
}

i64 getsid(u32 pid) {
    ScopedLock guard(g_tree_lock);
    const Process* process = find_locked(pid);
    return process ? static_cast<i64>(process->sid) : -SIRIUS_ESRCH;
}

i64 getpgid(u32 pid) {
    ScopedLock guard(g_tree_lock);
    const Process* process = find_locked(pid);
    return process ? static_cast<i64>(process->pgid) : -SIRIUS_ESRCH;
}

i64 setpgid(u32 pid, u32 pgid) {
    Process* self = current_process();
    ScopedLock guard(g_tree_lock);
    Process* process = find_locked(pid);
    if (process == nullptr || (process != self && process->parent != self)) return -SIRIUS_ESRCH;
    if (process->sid != self->sid || process->sid == process->pid) return -SIRIUS_EPERM;
    if (pgid == 0) pgid = process->pid;
    if (pgid != process->pid) {
        // Joining a group: it must exist, in the same session.
        bool exists = false;
        for (Process* other = g_all; other; other = other->all_next)
            if (other->pgid == pgid && other->sid == self->sid && other->state != ProcessState::Zombie) exists = true;
        if (!exists) return -SIRIUS_EPERM;
    }
    process->pgid = pgid;
    return 0;
}

u64 list_processes(sirius_procinfo* out, u64 capacity) {
    ScopedLock guard(g_tree_lock);
    u64 count = 0;
    auto describe = [&](const Process* process, u32 state) {
        sirius_procinfo& info = out[count++];
        info = {};
        info.pid = process->pid;
        info.parent = process->parent ? process->parent->pid : 0;
        info.state = state;
        str_copy(info.name, sizeof(info.name), process->name);
        return &info;
    };
    for (Process* process = g_all; process && count < capacity; process = process->all_next) {
        // A process is running if any of its threads is, and has used the
        // CPU time of all of them, the departed included.
        ScopedLock threads(process->threads_lock);
        bool runnable = false;
        u64 ticks = process->exited_ticks;
        for (const Thread* thread = process->threads; thread; thread = thread->process_next) {
            runnable = runnable || thread->state == ThreadState::Running || thread->state == ThreadState::Ready;
            ticks += __atomic_load_n(&thread->run_ticks, __ATOMIC_RELAXED);
        }
        sirius_procinfo* info = describe(process, runnable ? SIRIUS_PROC_RUNNING : SIRIUS_PROC_SLEEPING);
        info->cpu_ticks = ticks;
        if (process->threads) info->cpu = process->threads->cpu->index;
    }
    // Zombies have left the list of the living; they hang off their parents.
    for (const Process* parent = g_all; parent && count < capacity; parent = parent->all_next)
        for (const Process* child = parent->children; child && count < capacity; child = child->sibling)
            if (child->state == ProcessState::Zombie) describe(child, SIRIUS_PROC_ZOMBIE);
    return count;
}

void expire_alarms(u64 now_ns) {
    ScopedLock guard(g_tree_lock);
    for (Process* process = g_all; process; process = process->all_next) {
        const u64 deadline = __atomic_load_n(&process->alarm_deadline_ns, __ATOMIC_ACQUIRE);
        if (deadline == 0 || now_ns < deadline) continue;
        __atomic_store_n(&process->alarm_deadline_ns,
                         process->alarm_interval_ns ? now_ns + process->alarm_interval_ns : 0, __ATOMIC_RELEASE);
        signal::post(process, SIRIUS_SIGALRM);
    }
}

i64 sleep_interruptible(u64 milliseconds) {
    const u64 deadline = clock::now_ns() + milliseconds * 1'000'000;
    for (;;) {
        const int status = events::wait(events::snapshot(), deadline);
        if (status == -SIRIUS_ETIMEDOUT) return 0;
        if (status == -SIRIUS_EINTR) return -SIRIUS_EINTR;
    }
}

u32 parent_pid(Process* process) {
    ScopedLock guard(g_tree_lock);
    return process->parent ? process->parent->pid : 0;
}

u32 live_processes() { return __atomic_load_n(&g_live, __ATOMIC_RELAXED); }

[[noreturn]] void kill_current_for_fault(const Registers* regs) {
    Process* process = current_process();
    if (process == nullptr) panic_with_registers(regs, "exception in a kernel thread");

    if (regs->vector == 14) {
        LOG_ERROR("process %u '%s': killed by %s at rip=%p, address %p (error %#lx)",
                  process->pid, process->name, idt::exception_name(regs->vector),
                  reinterpret_cast<void*>(regs->rip),
                  reinterpret_cast<void*>(cpu::read_cr2()), regs->error_code);
    } else {
        LOG_ERROR("process %u '%s': killed by %s at rip=%p (error %#lx)",
                  process->pid, process->name, idt::exception_name(regs->vector),
                  reinterpret_cast<void*>(regs->rip), regs->error_code);
    }
    // Closing files may block on the disk: this is the thread's own kernel
    // stack, entered from ring 3, so it may sleep like any system call.
    irq::restore(1 << 9);
    exit_process(signal_for_vector(regs->vector));
}

void fault_in_user_mode(Registers* regs) {
    Process* process = current_process();
    if (process == nullptr) panic_with_registers(regs, "exception in a kernel thread");
    // A program that installed a handler for this gets to run it. One that
    // did not dies, with the report above.
    const i32 sig = signal_for_vector(regs->vector);
    const bool handled = process->sig_actions[sig].handler > SIRIUS_SIG_IGN &&
                         !(current()->sig_blocked & (1ULL << (sig - 1)));
    if (!handled) kill_current_for_fault(regs);
    signal::raise_fault(regs, sig);
}

} // namespace sched
