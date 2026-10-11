// Pipes: a byte stream from the descriptors holding the write end to those
// holding the read end, through a fixed buffer in the kernel.
#include <ipc/ipc.hpp>
#include <fs/vfs.hpp>
#include <sched/events.hpp>
#include <sched/process.hpp>
#include <sched/signal.hpp>
#include <mm/heap.hpp>
#include <mm/usercopy.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>
#include <sirius/fs.h>
#include <sirius/poll.h>

namespace ipc {
namespace {

constexpr u64 kCapacity = 64 * 1024;
// POSIX's PIPE_BUF: a write of up to this many bytes is never interleaved
// with another writer's.
constexpr u64 kAtomic = 4096;

struct Pipe {
    u8* buffer;
    u64 head;      // next byte to read
    u64 count;
    bool reader;   // is the read end still open anywhere?
    bool writer;
    u32 ends;      // 2, 1, then 0 and freed
};

class PipeEnd final : public vfs::Vnode {
public:
    PipeEnd(Pipe* pipe, bool write_end)
        : Vnode(vfs::anonymous_fs(), vfs::NodeType::Fifo, 0), m_pipe(pipe), m_write_end(write_end) {}

    // The last descriptor on this end is gone.
    ~PipeEnd() override {
        bool last;
        {
            ScopedLock guard(g_lock);
            (m_write_end ? m_pipe->writer : m_pipe->reader) = false;
            last = --m_pipe->ends == 0;
        }
        if (last) { kfree(m_pipe->buffer); kfree(m_pipe); }
        events::notify();   // readers see end of file, writers EPIPE
    }

    i64 read(u64, void* buffer, u64 length, u32 open_flags) override {
        if (m_write_end) return -SIRIUS_EBADF;
        if (length == 0) return 0;
        for (;;) {
            const u64 snapshot = events::snapshot();
            u8 chunk[kAtomic];
            u64 n = 0;
            bool end_of_file = false;
            {
                ScopedLock guard(g_lock);
                n = m_pipe->count < length ? m_pipe->count : length;
                if (n > sizeof(chunk)) n = sizeof(chunk);
                for (u64 i = 0; i < n; i++) chunk[i] = m_pipe->buffer[(m_pipe->head + i) % kCapacity];
                m_pipe->head = (m_pipe->head + n) % kCapacity;
                m_pipe->count -= n;
                end_of_file = n == 0 && !m_pipe->writer;
            }
            if (n) {
                // A read returns what there is; it does not wait to fill
                // the caller's buffer.
                memcpy(buffer, chunk, n);
                events::notify();   // there is room again
                return static_cast<i64>(n);
            }
            if (end_of_file) return 0;
            if (const i64 status = block(snapshot, open_flags & SIRIUS_O_NONBLOCK)) return status;
        }
    }

    i64 write(u64, const void* buffer, u64 length, u32 open_flags) override {
        if (!m_write_end) return -SIRIUS_EBADF;
        const auto* in = static_cast<const u8*>(buffer);
        u64 done = 0;
        while (done < length) {
            const u64 snapshot = events::snapshot();
            u8 chunk[kAtomic];
            const u64 want = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
            memcpy(chunk, in + done, want);

            u64 n = 0;
            bool broken = false;
            {
                ScopedLock guard(g_lock);
                broken = !m_pipe->reader;
                const u64 space = kCapacity - m_pipe->count;
                // All of a small write or none of it; as much as fits of a large one.
                if (!broken && space >= (length <= kAtomic ? want : 1)) {
                    n = want < space ? want : space;
                    const u64 tail = (m_pipe->head + m_pipe->count) % kCapacity;
                    for (u64 i = 0; i < n; i++) m_pipe->buffer[(tail + i) % kCapacity] = chunk[i];
                    m_pipe->count += n;
                }
            }
            if (broken) {
                signal::post(sched::current_process(), SIRIUS_SIGPIPE);
                return done ? static_cast<i64>(done) : -SIRIUS_EPIPE;
            }
            if (n) {
                done += n;
                events::notify();
                continue;
            }
            if (const i64 status = block(snapshot, open_flags & SIRIUS_O_NONBLOCK))
                return done ? static_cast<i64>(done) : status;
        }
        return static_cast<i64>(done);
    }

    u32 poll(u64&) override {
        ScopedLock guard(g_lock);
        u32 ready = 0;
        if (m_write_end) {
            if (!m_pipe->reader) ready |= SIRIUS_POLLERR;
            else if (kCapacity - m_pipe->count >= kAtomic) ready |= SIRIUS_POLLOUT;
        } else {
            if (m_pipe->count) ready |= SIRIUS_POLLIN;
            if (!m_pipe->writer) ready |= SIRIUS_POLLHUP;
        }
        return ready;
    }

private:
    Pipe* m_pipe;
    bool m_write_end;
};

} // namespace

i64 sys_pipe(u64 fds_address, u64 flags) {
    if (flags & ~static_cast<u64>(SIRIUS_O_CLOEXEC | SIRIUS_O_NONBLOCK)) return -SIRIUS_EINVAL;
    if (!usercopy::writable(fds_address, 2 * sizeof(i32))) return -SIRIUS_EFAULT;

    auto* pipe = static_cast<Pipe*>(kzalloc(sizeof(Pipe)));
    if (pipe == nullptr) return -SIRIUS_ENOMEM;
    pipe->buffer = static_cast<u8*>(kmalloc(kCapacity));
    if (pipe->buffer == nullptr) { kfree(pipe); return -SIRIUS_ENOMEM; }
    pipe->reader = pipe->writer = true;
    pipe->ends = 2;

    const i64 read_fd = install(new PipeEnd(pipe, false), SIRIUS_O_RDONLY, flags);
    const i64 write_fd = install(new PipeEnd(pipe, true), SIRIUS_O_WRONLY, flags);
    if (read_fd < 0 || write_fd < 0) {
        // Closing whichever end did get a descriptor releases it; an end
        // that did not was released by install().
        sched::Process* process = sched::current_process();
        const i64 made[2] = { read_fd, write_fd };
        for (const i64 fd : made) {
            if (fd < 0) continue;
            vfs::file_close(sched::detach_fd(process, static_cast<u64>(fd)));
        }
        return read_fd < 0 ? read_fd : write_fd;
    }

    const i32 fds[2] = { static_cast<i32>(read_fd), static_cast<i32>(write_fd) };
    usercopy::copy_to(fds_address, fds, sizeof(fds));
    return 0;
}

} // namespace ipc
