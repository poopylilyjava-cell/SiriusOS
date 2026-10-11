// Waiting: poll and epoll, and the descriptors made to be waited on --
// timerfd, signalfd, eventfd. A Wayland event loop is an epoll set holding
// its sockets, one timerfd and one signalfd.
#include <ipc/ipc.hpp>
#include <ipc/epoll.hpp>
#include <fs/vfs.hpp>
#include <sched/events.hpp>
#include <sched/process.hpp>
#include <sched/signal.hpp>
#include <arch/x86_64/clock.hpp>
#include <mm/heap.hpp>
#include <mm/usercopy.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>
#include <sirius/fs.h>
#include <sirius/poll.h>

namespace ipc {
namespace {

constexpr u64 kNever = ~0ULL;
constexpr u32 kAlwaysReported = SIRIUS_POLLERR | SIRIUS_POLLHUP;
constexpr u64 kMaxPollFds = 256;
constexpr u32 kEpollBatch = 64;

// A timeout in milliseconds as a deadline; negative waits for ever.
u64 deadline_after(i64 timeout_ms) {
    return timeout_ms < 0 ? 0 : clock::now_ns() + static_cast<u64>(timeout_ms) * 1'000'000;
}

// Sleep until something happens, the caller's deadline, or the moment a
// polled timer said it would fire -- whichever is first. Returns true when
// the caller's own deadline has passed, a negative errno for a signal.
i64 wait_for_events(u64 snapshot, u64 deadline_ns, u64 timer_ns, bool& timed_out) {
    timed_out = false;
    u64 until = deadline_ns;
    if (timer_ns != kNever && (until == 0 || timer_ns < until)) until = timer_ns;
    const int status = events::wait(snapshot, until);
    if (status == -SIRIUS_EINTR) return -SIRIUS_EINTR;
    if (deadline_ns && clock::now_ns() >= deadline_ns) timed_out = true;
    return 0;
}

// --- epoll ------------------------------------------------------------------------

struct Watch {
    Watch* next;
    vfs::File* file;   // not a reference: epoll::forget() removes it when the file goes
    i32 fd;
    u32 events;
    u64 data;
};

class Epoll;
Epoll* g_epolls = nullptr;   // all of them, for forget(); under g_lock

class Epoll final : public vfs::Vnode {
public:
    Epoll() : Vnode(vfs::anonymous_fs(), vfs::NodeType::Regular, 0) {
        ScopedLock guard(g_lock);
        next = g_epolls;
        g_epolls = this;
    }
    ~Epoll() override {
        ScopedLock guard(g_lock);
        for (Epoll** link = &g_epolls; *link; link = &(*link)->next)
            if (*link == this) { *link = next; break; }
        while (Watch* watch = watches) { watches = watch->next; kfree(watch); }
    }

    i64 read(u64, void*, u64) override { return -SIRIUS_EINVAL; }
    i64 write(u64, const void*, u64) override { return -SIRIUS_EINVAL; }

    // Up to `max` ready watches into `out`. A watched file's poll() may take
    // g_lock itself, so the watches are copied out under the lock -- with a
    // reference each, in case another process closes one meanwhile -- and
    // asked afterwards.
    u32 collect(sirius_epoll_event* out, u32 max, u64& wake_at_ns) {
        u32 found = 0;
        for (u32 start = 0; found < max;) {
            Watch batch[kEpollBatch];
            u32 n = 0;
            bool more = false;
            {
                ScopedLock guard(g_lock);
                u32 index = 0;
                for (Watch* watch = watches; watch; watch = watch->next, index++) {
                    if (index < start) continue;
                    if (n == kEpollBatch) { more = true; break; }
                    if (vfs::file_try_ref(watch->file)) batch[n++] = *watch;
                }
                start = index;
            }
            for (u32 i = 0; i < n; i++) {
                const u32 ready = vfs::poll(batch[i].file, wake_at_ns) & (batch[i].events | kAlwaysReported);
                if (ready && found < max) out[found++] = { ready, batch[i].data };
                vfs::file_close(batch[i].file);
            }
            if (!more) break;
        }
        return found;
    }

    // An epoll descriptor is readable when anything it watches is ready.
    u32 poll(u64& wake_at_ns) override {
        sirius_epoll_event one;
        return collect(&one, 1, wake_at_ns) ? SIRIUS_POLLIN : 0;
    }

    Epoll* next = nullptr;
    Watch* watches = nullptr;
};

Epoll* epoll_of(vfs::File* file) {
    if (file == nullptr) return nullptr;
    ScopedLock guard(g_lock);
    for (Epoll* e = g_epolls; e; e = e->next)
        if (e == file->vnode) return e;
    return nullptr;
}

// timerfd_settime and signalfd must tell their own kind of descriptor from
// any other. There is no RTTI in the kernel; a list of which node is what is
// simpler than a type field in every Vnode for the sake of two classes.
enum class Kind : u8 { Timer, Signal };
struct Tagged { Tagged* next; vfs::Vnode* node; Kind kind; };
Tagged* g_tagged = nullptr;   // under g_lock

void tag(vfs::Vnode* node, Kind kind) {
    auto* entry = static_cast<Tagged*>(kzalloc(sizeof(Tagged)));
    if (entry == nullptr) return;
    entry->node = node;
    entry->kind = kind;
    ScopedLock guard(g_lock);
    entry->next = g_tagged;
    g_tagged = entry;
}

void untag(vfs::Vnode* node) {
    Tagged* found = nullptr;
    {
        ScopedLock guard(g_lock);
        for (Tagged** link = &g_tagged; *link; link = &(*link)->next)
            if ((*link)->node == node) { found = *link; *link = found->next; break; }
    }
    kfree(found);
}

bool is_kind(vfs::File* file, Kind kind) {
    if (file == nullptr) return false;
    ScopedLock guard(g_lock);
    for (Tagged* entry = g_tagged; entry; entry = entry->next)
        if (entry->node == file->vnode) return entry->kind == kind;
    return false;
}

// --- timerfd ----------------------------------------------------------------------

class Timer final : public vfs::Vnode {
public:
    Timer() : Vnode(vfs::anonymous_fs(), vfs::NodeType::Regular, 0) {}
    ~Timer() override { untag(this); }

    // How many times it has fired since the last read: 8 bytes.
    i64 read(u64, void* buffer, u64 length, u32 open_flags) override {
        if (length < sizeof(u64)) return -SIRIUS_EINVAL;
        for (;;) {
            const u64 snapshot = events::snapshot();
            u64 count = 0, expiry;
            {
                ScopedLock guard(g_lock);
                const u64 now = clock::now_ns();
                expiry = m_expiry;
                if (expiry && now >= expiry) {
                    count = 1;
                    if (m_interval) {
                        count += (now - expiry) / m_interval;
                        m_expiry = expiry + count * m_interval;
                    } else {
                        m_expiry = 0;
                    }
                }
            }
            if (count) { memcpy(buffer, &count, sizeof(count)); return sizeof(count); }
            if (const i64 status = block(snapshot, open_flags & SIRIUS_O_NONBLOCK, expiry)) return status;
        }
    }

    u32 poll(u64& wake_at_ns) override {
        ScopedLock guard(g_lock);
        if (m_expiry == 0) return 0;
        if (clock::now_ns() >= m_expiry) return SIRIUS_POLLIN;
        if (m_expiry < wake_at_ns) wake_at_ns = m_expiry;
        return 0;
    }

    void set(u64 expiry, u64 interval, sirius_itimerspec* old) {
        ScopedLock guard(g_lock);
        if (old) {
            const u64 now = clock::now_ns();
            const u64 left = m_expiry > now ? m_expiry - now : (m_expiry ? 1 : 0);
            *old = { { static_cast<i64>(m_interval / 1'000'000'000), static_cast<i64>(m_interval % 1'000'000'000) },
                     { static_cast<i64>(left / 1'000'000'000), static_cast<i64>(left % 1'000'000'000) } };
        }
        m_expiry = expiry;
        m_interval = interval;
    }

private:
    u64 m_expiry = 0;     // on the clock:: time base; 0 when disarmed
    u64 m_interval = 0;
};

// --- signalfd ---------------------------------------------------------------------

// Reads the calling process's pending signals out of its signal state, so
// that a program can take them from its event loop instead of in a handler.
// The program blocks the signals it wants delivered this way.
class SignalFd final : public vfs::Vnode {
public:
    explicit SignalFd(u64 mask) : Vnode(vfs::anonymous_fs(), vfs::NodeType::Regular, 0), mask(mask) {}
    ~SignalFd() override { untag(this); }

    i64 read(u64, void* buffer, u64 length, u32 open_flags) override {
        const u64 room = length / sizeof(sirius_signalfd_siginfo);
        if (room == 0) return -SIRIUS_EINVAL;
        sched::Process* process = sched::current_process();
        for (;;) {
            const u64 snapshot = events::snapshot();
            const u64 wanted = __atomic_load_n(&mask, __ATOMIC_RELAXED);
            u64 done = 0;
            while (done < room) {
                const u64 ready = __atomic_load_n(&process->sig_pending, __ATOMIC_ACQUIRE) & wanted;
                if (ready == 0) break;
                const int sig = __builtin_ctzll(ready) + 1;
                // Only whoever clears the bit has taken the signal.
                const u64 before = __atomic_fetch_and(&process->sig_pending, ~(1ULL << (sig - 1)), __ATOMIC_ACQ_REL);
                if (!(before & (1ULL << (sig - 1)))) continue;
                sirius_signalfd_siginfo info{};
                info.signo = static_cast<u32>(sig);
                memcpy(static_cast<u8*>(buffer) + done * sizeof(info), &info, sizeof(info));
                done++;
            }
            if (done) return static_cast<i64>(done * sizeof(sirius_signalfd_siginfo));
            if (const i64 status = block(snapshot, open_flags & SIRIUS_O_NONBLOCK)) return status;
        }
    }

    u32 poll(u64&) override {
        const sched::Process* process = sched::current_process();
        return (__atomic_load_n(&process->sig_pending, __ATOMIC_ACQUIRE) & __atomic_load_n(&mask, __ATOMIC_RELAXED))
             ? SIRIUS_POLLIN : 0;
    }

    u64 mask;
};

// --- eventfd ----------------------------------------------------------------------

class EventFd final : public vfs::Vnode {
public:
    EventFd(u64 initial, bool semaphore)
        : Vnode(vfs::anonymous_fs(), vfs::NodeType::Regular, 0), m_count(initial), m_semaphore(semaphore) {}

    i64 read(u64, void* buffer, u64 length, u32 open_flags) override {
        if (length < sizeof(u64)) return -SIRIUS_EINVAL;
        for (;;) {
            const u64 snapshot = events::snapshot();
            u64 value = 0;
            {
                ScopedLock guard(g_lock);
                if (m_count) {
                    value = m_semaphore ? 1 : m_count;
                    m_count -= value;
                }
            }
            if (value) {
                memcpy(buffer, &value, sizeof(value));
                events::notify();
                return sizeof(value);
            }
            if (const i64 status = block(snapshot, open_flags & SIRIUS_O_NONBLOCK)) return status;
        }
    }

    i64 write(u64, const void* buffer, u64 length, u32 open_flags) override {
        if (length < sizeof(u64)) return -SIRIUS_EINVAL;
        u64 value;
        memcpy(&value, buffer, sizeof(value));
        if (value == ~0ULL) return -SIRIUS_EINVAL;
        for (;;) {
            const u64 snapshot = events::snapshot();
            bool added = false;
            {
                ScopedLock guard(g_lock);
                if (value <= ~0ULL - 1 - m_count) { m_count += value; added = true; }
            }
            if (added) { events::notify(); return sizeof(value); }
            if (const i64 status = block(snapshot, open_flags & SIRIUS_O_NONBLOCK)) return status;
        }
    }

    u32 poll(u64&) override {
        ScopedLock guard(g_lock);
        return (m_count ? SIRIUS_POLLIN : 0) | (m_count < ~0ULL - 1 ? SIRIUS_POLLOUT : 0);
    }

private:
    u64 m_count;
    bool m_semaphore;
};

} // namespace

// --- system calls -----------------------------------------------------------------

i64 sys_poll(u64 fds_address, u64 count, i64 timeout_ms) {
    if (count > kMaxPollFds) return -SIRIUS_EINVAL;
    const u64 bytes = count * sizeof(sirius_pollfd);
    if (count && !usercopy::writable(fds_address, bytes)) return -SIRIUS_EFAULT;

    auto* fds = static_cast<sirius_pollfd*>(kmalloc(bytes ? bytes : 1));
    if (fds == nullptr) return -SIRIUS_ENOMEM;
    if (count) memcpy(fds, reinterpret_cast<const void*>(fds_address), bytes);

    const u64 deadline = deadline_after(timeout_ms);
    i64 result;
    for (;;) {
        const u64 snapshot = events::snapshot();
        u64 timer = kNever;
        i64 ready = 0;
        for (u64 i = 0; i < count; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;   // a negative descriptor is skipped, by definition
            vfs::File* file = file_at(static_cast<u64>(fds[i].fd));
            if (file == nullptr) fds[i].revents = SIRIUS_POLLNVAL;
            else fds[i].revents = static_cast<i16>(vfs::poll(file, timer) & (static_cast<u32>(fds[i].events) | kAlwaysReported));
            if (fds[i].revents) ready++;
        }
        if (ready || timeout_ms == 0) { result = ready; break; }
        bool timed_out;
        if (const i64 status = wait_for_events(snapshot, deadline, timer, timed_out)) { result = status; break; }
        if (timed_out) { result = 0; break; }
    }
    if (result >= 0 && count) memcpy(reinterpret_cast<void*>(fds_address), fds, bytes);
    kfree(fds);
    return result;
}

i64 sys_epoll_create(u64 flags) {
    if (flags & ~static_cast<u64>(SIRIUS_O_CLOEXEC)) return -SIRIUS_EINVAL;
    return install(new Epoll(), SIRIUS_O_RDWR, flags);
}

i64 sys_epoll_ctl(u64 epfd, u64 op, u64 fd, u64 event_address) {
    Epoll* self = epoll_of(file_at(epfd));
    if (self == nullptr) return file_at(epfd) ? -SIRIUS_EINVAL : -SIRIUS_EBADF;
    vfs::File* target = file_at(fd);
    if (target == nullptr) return -SIRIUS_EBADF;
    if (target->vnode == self) return -SIRIUS_EINVAL;

    sirius_epoll_event event{};
    if (op != SIRIUS_EPOLL_CTL_DEL) {
        if (const int status = usercopy::copy_from(&event, event_address, sizeof(event)); status < 0) return status;
    }
    Watch* fresh = op == SIRIUS_EPOLL_CTL_ADD ? static_cast<Watch*>(kzalloc(sizeof(Watch))) : nullptr;
    if (op == SIRIUS_EPOLL_CTL_ADD && fresh == nullptr) return -SIRIUS_ENOMEM;

    i64 result = 0;
    Watch* removed = nullptr;
    {
        ScopedLock guard(g_lock);
        Watch** link = &self->watches;
        while (*link && !((*link)->file == target && (*link)->fd == static_cast<i32>(fd))) link = &(*link)->next;
        switch (op) {
        case SIRIUS_EPOLL_CTL_ADD:
            if (*link) { result = -SIRIUS_EEXIST; break; }
            *fresh = { nullptr, target, static_cast<i32>(fd), event.events, event.data };
            *link = fresh;   // at the tail: events are reported in the order they were added
            fresh = nullptr;
            break;
        case SIRIUS_EPOLL_CTL_MOD:
            if (*link == nullptr) { result = -SIRIUS_ENOENT; break; }
            (*link)->events = event.events;
            (*link)->data = event.data;
            break;
        case SIRIUS_EPOLL_CTL_DEL:
            if (*link == nullptr) { result = -SIRIUS_ENOENT; break; }
            removed = *link;
            *link = removed->next;
            break;
        default:
            result = -SIRIUS_EINVAL;
        }
    }
    kfree(fresh);
    kfree(removed);
    if (result == 0) events::notify();   // a waiter should look at the new set
    return result;
}

i64 sys_epoll_wait(u64 epfd, u64 events_address, u64 max_events, i64 timeout_ms, u64 sigmask_address) {
    vfs::File* file = file_at(epfd);
    Epoll* self = epoll_of(file);
    if (self == nullptr) return file ? -SIRIUS_EINVAL : -SIRIUS_EBADF;
    if (max_events == 0 || max_events > 4096) return -SIRIUS_EINVAL;
    if (!usercopy::writable(events_address, max_events * sizeof(sirius_epoll_event))) return -SIRIUS_EFAULT;

    if (sigmask_address) {
        u64 mask;
        if (const int status = usercopy::copy_from(&mask, sigmask_address, sizeof(mask)); status < 0) return status;
        signal::set_temporary_mask(mask);
    }

    const u32 max = max_events < kEpollBatch ? static_cast<u32>(max_events) : kEpollBatch;
    sirius_epoll_event found[kEpollBatch];
    const u64 deadline = deadline_after(timeout_ms);
    // The set must stay alive while it is searched, whatever happens to the
    // descriptor meanwhile.
    vfs::file_ref(file);
    i64 result;
    for (;;) {
        const u64 snapshot = events::snapshot();
        u64 timer = kNever;
        const u32 n = self->collect(found, max, timer);
        if (n || timeout_ms == 0) { result = n; break; }
        bool timed_out;
        if (const i64 status = wait_for_events(snapshot, deadline, timer, timed_out)) { result = status; break; }
        if (timed_out) { result = 0; break; }
    }
    vfs::file_close(file);

    if (result > 0) memcpy(reinterpret_cast<void*>(events_address), found, static_cast<u64>(result) * sizeof(sirius_epoll_event));
    // Interrupted: the signal is delivered under the temporary mask and the
    // old one restored after it. Otherwise it comes back now.
    if (sigmask_address && result != -SIRIUS_EINTR) signal::drop_temporary_mask();
    return result;
}

i64 sys_timerfd_create(u64 clock_id, u64 flags) {
    if (clock_id > 1) return -SIRIUS_EINVAL;   // realtime and monotonic are the same clock
    if (flags & ~static_cast<u64>(SIRIUS_O_CLOEXEC | SIRIUS_O_NONBLOCK)) return -SIRIUS_EINVAL;
    auto* timer = new Timer();
    tag(timer, Kind::Timer);
    const i64 fd = install(timer, SIRIUS_O_RDWR, flags);
    return fd;
}

i64 sys_timerfd_settime(u64 fd, u64 flags, u64 new_address, u64 old_address) {
    vfs::File* file = file_at(fd);
    if (file == nullptr) return -SIRIUS_EBADF;
    if (!is_kind(file, Kind::Timer)) return -SIRIUS_EINVAL;
    if (flags & ~static_cast<u64>(SIRIUS_TFD_TIMER_ABSTIME)) return -SIRIUS_EINVAL;

    sirius_itimerspec spec;
    if (const int status = usercopy::copy_from(&spec, new_address, sizeof(spec)); status < 0) return status;
    if (spec.value.sec < 0 || spec.value.nsec < 0 || spec.value.nsec >= 1'000'000'000 ||
        spec.interval.sec < 0 || spec.interval.nsec < 0 || spec.interval.nsec >= 1'000'000'000) return -SIRIUS_EINVAL;

    const u64 value = static_cast<u64>(spec.value.sec) * 1'000'000'000 + static_cast<u64>(spec.value.nsec);
    const u64 interval = static_cast<u64>(spec.interval.sec) * 1'000'000'000 + static_cast<u64>(spec.interval.nsec);
    u64 expiry = 0;
    if (value) {
        expiry = (flags & SIRIUS_TFD_TIMER_ABSTIME) ? value : clock::now_ns() + value;
        if (expiry == 0) expiry = 1;
    }

    sirius_itimerspec old{};
    static_cast<Timer*>(file->vnode)->set(expiry, interval, old_address ? &old : nullptr);
    events::notify();   // whoever sleeps on the old expiry must look again
    return old_address ? usercopy::copy_to(old_address, &old, sizeof(old)) : 0;
}

i64 sys_signalfd(i64 fd, u64 mask_address, u64 flags) {
    u64 mask;
    if (const int status = usercopy::copy_from(&mask, mask_address, sizeof(mask)); status < 0) return status;
    if (flags & ~static_cast<u64>(SIRIUS_O_CLOEXEC | SIRIUS_O_NONBLOCK)) return -SIRIUS_EINVAL;

    if (fd >= 0) {
        // Change the set of an existing one.
        vfs::File* file = file_at(static_cast<u64>(fd));
        if (file == nullptr) return -SIRIUS_EBADF;
        if (!is_kind(file, Kind::Signal)) return -SIRIUS_EINVAL;
        __atomic_store_n(&static_cast<SignalFd*>(file->vnode)->mask, mask, __ATOMIC_RELEASE);
        events::notify();
        return fd;
    }
    auto* node = new SignalFd(mask);
    tag(node, Kind::Signal);
    return install(node, SIRIUS_O_RDONLY, flags);
}

i64 sys_eventfd(u64 initial, u64 flags) {
    if (flags & ~static_cast<u64>(SIRIUS_O_CLOEXEC | SIRIUS_O_NONBLOCK | SIRIUS_EFD_SEMAPHORE)) return -SIRIUS_EINVAL;
    return install(new EventFd(initial & 0xffffffff, flags & SIRIUS_EFD_SEMAPHORE), SIRIUS_O_RDWR, flags);
}

} // namespace ipc

void epoll::forget(vfs::File* file) {
    ipc::Watch* dead = nullptr;
    {
        ScopedLock guard(ipc::g_lock);
        for (ipc::Epoll* e = ipc::g_epolls; e; e = e->next) {
            for (ipc::Watch** link = &e->watches; *link;) {
                if ((*link)->file != file) { link = &(*link)->next; continue; }
                ipc::Watch* watch = *link;
                *link = watch->next;
                watch->next = dead;
                dead = watch;
            }
        }
    }
    while (dead) { ipc::Watch* next = dead->next; kfree(dead); dead = next; }
}
