#include <drivers/input.hpp>
#include <arch/x86_64/clock.hpp>
#include <fs/devfs.hpp>
#include <sched/waitqueue.hpp>
#include <klib/spinlock.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>
#include <sirius/fs.h>
#include <sirius/input.h>
#include <sirius/poll.h>
#include <sched/events.hpp>

namespace input {
namespace {

constexpr u32 kCapacity = 1024;   // a power of two

Spinlock g_lock;
sirius_input_event g_events[kCapacity];
u32 g_head = 0;   // next to read
u32 g_count = 0;
sched::WaitQueue g_readers;

class InputDevice final : public vfs::Vnode {
public:
    explicit InputDevice(vfs::Filesystem* fs) : Vnode(fs, vfs::NodeType::CharDevice, 5) {}

    // Whole records only. With nothing queued: wait, or with O_NONBLOCK
    // return EAGAIN.
    i64 read(u64, void* buffer, u64 length, u32 open_flags) override {
        const u64 wanted = length / sizeof(sirius_input_event);
        if (wanted == 0) return -SIRIUS_EINVAL;

        // `buffer` is user memory, and touching it may fault a page in --
        // which must not happen with a spinlock held and interrupts off. So
        // events leave the queue through a small buffer on this stack.
        sirius_input_event batch[16];
        u64 flags = g_lock.lock_irqsave();
        while (g_count == 0) {
            if (open_flags & SIRIUS_O_NONBLOCK) { g_lock.unlock_irqrestore(flags); return -SIRIUS_EAGAIN; }
            g_readers.wait(g_lock, flags);
            flags = g_lock.lock_irqsave();
        }
        u64 n = wanted < 16 ? wanted : 16;
        if (n > g_count) n = g_count;
        for (u64 i = 0; i < n; i++) batch[i] = g_events[(g_head + i) % kCapacity];
        g_head = (g_head + static_cast<u32>(n)) % kCapacity;
        g_count -= static_cast<u32>(n);
        g_lock.unlock_irqrestore(flags);

        memcpy(buffer, batch, n * sizeof(sirius_input_event));
        return static_cast<i64>(n * sizeof(sirius_input_event));
    }

    i64 write(u64, const void*, u64) override { return -SIRIUS_EACCES; }

    u32 poll(u64&) override {
        ScopedLock guard(g_lock);
        return g_count ? SIRIUS_POLLIN : 0;
    }
};

} // namespace

void init() {
    devfs::add("input", new InputDevice(devfs::instance()));
}

void post(u16 type, u16 code, i32 value) {
    const u64 now = clock::now_ns();
    ScopedLock guard(g_lock);
    if (g_count == kCapacity) {
        g_head = (g_head + 1) % kCapacity;
        g_count--;
    }
    g_events[(g_head + g_count) % kCapacity] = { now, type, code, value };
    g_count++;
    g_readers.wake_all();
    // For whoever sleeps in poll() rather than in read().
    events::notify();
}

} // namespace input
