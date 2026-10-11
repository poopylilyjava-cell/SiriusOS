#pragma once
#include <klib/types.hpp>
#include <klib/spinlock.hpp>

namespace vfs { struct File; class Vnode; }

// Inter-process communication: the objects behind descriptors that are not
// files on a disk -- pipes, unix sockets, shared memory, and the descriptors
// that exist to be waited on. What Wayland needs of a kernel (ADR 0004).
//
// They all block the same way (sched/events.hpp), and one spinlock guards
// the state of all of them: none holds it for longer than it takes to move
// a few kilobytes between kernel buffers, and one lock cannot be taken in
// the wrong order. User memory is never touched with it held -- data crosses
// through a kernel buffer -- because touching user memory can fault.
namespace ipc {

extern Spinlock g_lock;

// Nothing to do right now. Returns 0 once it is worth looking again,
// -EAGAIN at once for a non-blocking caller, or signal::kRestart if a signal
// arrived instead. `snapshot` must predate the look that found nothing.
i64 block(u64 snapshot, bool nonblocking, u64 deadline_ns = 0);

// --- system calls. Each returns a descriptor, a count, 0, or -errno. ---
i64 sys_pipe(u64 fds_address, u64 flags);
i64 sys_openpty(u64 fds_address, u64 termios_address, u64 winsize_address);
i64 sys_memfd_create(u64 name_address, u64 flags);

i64 sys_socket(u64 domain, u64 type, u64 protocol);
i64 sys_socketpair(u64 domain, u64 type, u64 protocol, u64 fds_address);
i64 sys_bind(u64 fd, u64 address, u64 length);
i64 sys_listen(u64 fd, u64 backlog);
i64 sys_accept(u64 fd, u64 flags);
i64 sys_connect(u64 fd, u64 address, u64 length);
i64 sys_sendmsg(u64 fd, u64 message_address, u64 flags);
i64 sys_recvmsg(u64 fd, u64 message_address, u64 flags);
i64 sys_shutdown(u64 fd, u64 how);
i64 sys_getsockopt(u64 fd, u64 level, u64 name, u64 value_address, u64 length_address);

i64 sys_poll(u64 fds_address, u64 count, i64 timeout_ms);
i64 sys_epoll_create(u64 flags);
i64 sys_epoll_ctl(u64 epfd, u64 op, u64 fd, u64 event_address);
i64 sys_epoll_wait(u64 epfd, u64 events_address, u64 max_events, i64 timeout_ms, u64 sigmask_address);
i64 sys_timerfd_create(u64 clock, u64 flags);
i64 sys_timerfd_settime(u64 fd, u64 flags, u64 new_address, u64 old_address);
i64 sys_signalfd(i64 fd, u64 mask_address, u64 flags);
i64 sys_eventfd(u64 initial, u64 flags);

// A new shared memory object of `size` bytes with no descriptor, for
// mmap(MAP_SHARED | MAP_ANONYMOUS): memory a process shares with the children
// it forks. One reference, the caller's; null if out of memory.
vfs::Vnode* create_shared_memory(u64 size);

// Helpers shared by the files in this directory.
vfs::File* file_at(u64 fd);                                     // the caller's descriptor, or null
i64 install(vfs::Vnode* vnode, u32 open_flags, u64 creation_flags);  // wrap a new node in a descriptor; flags may carry O_CLOEXEC
u32 open_flags_from(u64 creation_flags);                        // the O_NONBLOCK part of such flags

} // namespace ipc
