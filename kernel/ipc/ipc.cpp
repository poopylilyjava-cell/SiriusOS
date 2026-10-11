#include <ipc/ipc.hpp>
#include <fs/vfs.hpp>
#include <sched/events.hpp>
#include <sched/process.hpp>
#include <sched/signal.hpp>
#include <sirius/errno.h>
#include <sirius/fs.h>
#include <sirius/poll.h>

namespace ipc {

Spinlock g_lock;

i64 block(u64 snapshot, bool nonblocking, u64 deadline_ns) {
    if (nonblocking) return -SIRIUS_EAGAIN;
    const int status = events::wait(snapshot, deadline_ns);
    if (status == -SIRIUS_EINTR) return signal::kRestart;
    return 0;   // woken, or a deadline passed: either way, look again
}

vfs::File* file_at(u64 fd) { return sched::file_of(fd); }

u32 open_flags_from(u64 creation_flags) {
    return static_cast<u32>(creation_flags) & SIRIUS_O_NONBLOCK;
}

i64 install(vfs::Vnode* vnode, u32 open_flags, u64 creation_flags) {
    vfs::File* file = vfs::open_anonymous(vnode, open_flags | open_flags_from(creation_flags));
    if (file == nullptr) return -SIRIUS_ENOMEM;
    const i64 fd = sched::install_fd(sched::current_process(), file,
                                     (creation_flags & SIRIUS_O_CLOEXEC) ? SIRIUS_FD_CLOEXEC : 0);
    if (fd < 0) vfs::file_close(file);
    return fd;
}

} // namespace ipc
