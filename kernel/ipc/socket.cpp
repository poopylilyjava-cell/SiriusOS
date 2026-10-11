// Unix domain stream sockets, with file descriptor passing (SCM_RIGHTS).
//
// A connection is two Socket objects pointing at each other. Sending puts a
// segment on the peer's receive queue; receiving takes bytes off the front of
// one's own. A segment may carry open files: the descriptors a sender
// attaches become references held by the segment while in flight, and new
// descriptors in whichever process receives its first byte. That is the whole
// of how a Wayland client hands the compositor a buffer.
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
#include <sirius/socket.h>

namespace ipc {
namespace {

constexpr u64 kReceiveCapacity = 256 * 1024;   // bytes queued for one socket before senders wait
constexpr u64 kSegmentMax = 32 * 1024;
constexpr u32 kMaxBacklog = 128;
constexpr u64 kMaxIov = 64;

struct Segment {
    Segment* next;
    u32 length;
    u32 consumed;
    u32 file_count;
    vfs::File** files;      // in flight; delivered with the segment's first byte
    u8 data[];
};

enum class State : u8 { Unconnected, Listening, Connected };

class Socket;
Socket* g_listeners = nullptr;   // every listening socket, to be found by its name's node

class Socket final : public vfs::Vnode {
public:
    Socket() : Vnode(vfs::anonymous_fs(), vfs::NodeType::Socket, 0) {}
    ~Socket() override;

    i64 read(u64, void* buffer, u64 length, u32 open_flags) override;
    i64 write(u64, const void* buffer, u64 length, u32 open_flags) override;
    u32 poll(u64&) override;

    // Everything below is guarded by ipc::g_lock.
    State state = State::Unconnected;
    Socket* peer = nullptr;         // null once the other side is gone
    bool was_connected = false;     // tells "never connected" from "peer closed"
    bool read_shut = false;         // shutdown(SHUT_RD): our own choice
    bool write_shut = false;        // shutdown(SHUT_WR)
    bool peer_done = false;         // nothing more will arrive: the peer closed or shut down its writing
    Segment* receive_head = nullptr;
    Segment* receive_tail = nullptr;
    u64 receive_bytes = 0;
    u32 peer_pid = 0;               // for SO_PEERCRED

    // A listening socket.
    vfs::Vnode* name = nullptr;     // its node in the filesystem; one reference
    Socket* next_listener = nullptr;
    Socket* accept_head = nullptr;  // connections made and not yet accepted, linked by `next_pending`
    Socket* accept_tail = nullptr;
    Socket* next_pending = nullptr;
    u32 pending = 0;
    u32 backlog = 0;
};

Socket* socket_of(vfs::File* file) {
    if (file == nullptr || file->vnode->type() != vfs::NodeType::Socket || file->vnode->fs() != vfs::anonymous_fs()) return nullptr;
    return static_cast<Socket*>(file->vnode);
}

void free_segment(Segment* segment) {
    for (u32 i = 0; i < segment->file_count; i++) vfs::file_close(segment->files[i]);
    kfree(segment->files);
    kfree(segment);
}

// Make two sockets each other's peer. Caller holds the lock.
void connect_pair(Socket* a, Socket* b, u32 a_pid, u32 b_pid) {
    a->state = b->state = State::Connected;
    a->was_connected = b->was_connected = true;
    a->peer = b; b->peer = a;
    a->peer_pid = b_pid; b->peer_pid = a_pid;
}

Socket::~Socket() {
    Segment* segments;
    Socket* unaccepted;
    vfs::Vnode* bound;
    {
        ScopedLock guard(g_lock);
        if (peer) {
            peer->peer = nullptr;
            peer->peer_done = true;
        }
        if (state == State::Listening)
            for (Socket** link = &g_listeners; *link; link = &(*link)->next_listener)
                if (*link == this) { *link = next_listener; break; }
        segments = receive_head;
        unaccepted = accept_head;
        bound = name;
        receive_head = receive_tail = nullptr;
        accept_head = accept_tail = nullptr;
    }
    // Outside the lock: closing a file in flight may destroy another socket.
    while (segments) { Segment* next = segments->next; free_segment(segments); segments = next; }
    // Connections nobody accepted: their clients see the peer close.
    while (unaccepted) { Socket* next = unaccepted->next_pending; unaccepted->unref(); unaccepted = next; }
    if (bound) bound->unref();
    events::notify();
}

// Take up to `capacity` bytes off the receive queue into `out`, and with them
// the files attached to the first segment touched. Returns the byte count;
// 0 with `end` set means the stream is over. -EAGAIN / kRestart as block().
i64 receive(Socket* self, u8* out, u64 capacity, bool nonblocking, vfs::File**& files, u32& file_count) {
    files = nullptr;
    file_count = 0;
    for (;;) {
        const u64 snapshot = events::snapshot();
        u64 taken = 0;
        bool end = false, unconnected = false;
        Segment* spent = nullptr;
        {
            ScopedLock guard(g_lock);
            unconnected = self->state != State::Connected;
            while (!unconnected && self->receive_head && taken < capacity) {
                Segment* segment = self->receive_head;
                // Files travel with the first byte of their segment, and one
                // receive delivers one segment's worth of them: stop before
                // a second batch.
                if (segment->file_count && segment->consumed == 0) {
                    if (taken > 0 || files != nullptr) break;
                    files = segment->files;
                    file_count = segment->file_count;
                    segment->files = nullptr;
                    segment->file_count = 0;
                }
                u64 n = segment->length - segment->consumed;
                if (n > capacity - taken) n = capacity - taken;
                memcpy(out + taken, segment->data + segment->consumed, n);
                segment->consumed += static_cast<u32>(n);
                taken += n;
                self->receive_bytes -= n;
                if (segment->consumed == segment->length) {
                    self->receive_head = segment->next;
                    if (self->receive_head == nullptr) self->receive_tail = nullptr;
                    segment->next = spent;
                    spent = segment;
                }
            }
            end = taken == 0 && (self->peer_done || self->read_shut);
        }
        while (spent) { Segment* next = spent->next; free_segment(spent); spent = next; }

        if (unconnected) return -SIRIUS_ENOTCONN;
        if (taken) { events::notify(); return static_cast<i64>(taken); }   // senders waiting for room
        if (end) return 0;
        if (const i64 status = block(snapshot, nonblocking)) return status;
    }
}

// Queue `length` bytes from `data` (kernel memory) for the peer, the files
// riding on the first of them. Takes over the `files` array and its
// references whatever happens.
i64 transmit(Socket* self, const u8* data, u64 length, bool nonblocking, bool raise_sigpipe, vfs::File** files, u32 file_count) {
    auto drop_files = [&] {
        for (u32 i = 0; i < file_count; i++) vfs::file_close(files[i]);
        kfree(files);
        files = nullptr;
        file_count = 0;
    };

    u64 done = 0;
    while (done < length) {
        const u64 n = length - done < kSegmentMax ? length - done : kSegmentMax;
        auto* segment = static_cast<Segment*>(kmalloc(sizeof(Segment) + n));
        if (segment == nullptr) { drop_files(); return done ? static_cast<i64>(done) : -SIRIUS_ENOMEM; }
        segment->next = nullptr;
        segment->length = static_cast<u32>(n);
        segment->consumed = 0;
        segment->files = nullptr;
        segment->file_count = 0;
        memcpy(segment->data, data + done, n);

        for (;;) {
            const u64 snapshot = events::snapshot();
            enum { Queued, Full, Broken, Unconnected } outcome;
            {
                ScopedLock guard(g_lock);
                Socket* peer = self->peer;
                if (self->state != State::Connected && !self->was_connected) outcome = Unconnected;
                else if (peer == nullptr || self->write_shut || peer->read_shut) outcome = Broken;
                // An empty queue always takes a segment, so that one larger
                // than the capacity can still be sent.
                else if (peer->receive_bytes && peer->receive_bytes + n > kReceiveCapacity) outcome = Full;
                else {
                    if (done == 0) { segment->files = files; segment->file_count = file_count; files = nullptr; file_count = 0; }
                    if (peer->receive_tail) peer->receive_tail->next = segment;
                    else peer->receive_head = segment;
                    peer->receive_tail = segment;
                    peer->receive_bytes += n;
                    outcome = Queued;
                }
            }
            if (outcome == Queued) break;
            if (outcome == Full) {
                const i64 status = block(snapshot, nonblocking);
                if (status == 0) continue;
                kfree(segment);
                drop_files();
                return done ? static_cast<i64>(done) : status;
            }
            kfree(segment);
            drop_files();
            if (outcome == Unconnected) return -SIRIUS_ENOTCONN;
            if (raise_sigpipe) signal::post(sched::current_process(), SIRIUS_SIGPIPE);
            return done ? static_cast<i64>(done) : -SIRIUS_EPIPE;
        }
        done += n;
        events::notify();
    }
    drop_files();   // a zero-length send carries nothing
    return static_cast<i64>(done);
}

i64 Socket::read(u64, void* buffer, u64 length, u32 open_flags) {
    if (length == 0) return 0;
    const u64 capacity = length < kSegmentMax ? length : kSegmentMax;
    auto* bounce = static_cast<u8*>(kmalloc(capacity));
    if (bounce == nullptr) return -SIRIUS_ENOMEM;
    vfs::File** files;
    u32 file_count;
    const i64 n = receive(this, bounce, capacity, open_flags & SIRIUS_O_NONBLOCK, files, file_count);
    if (n > 0) memcpy(buffer, bounce, static_cast<u64>(n));
    kfree(bounce);
    // A plain read has nowhere to put descriptors: they are closed.
    for (u32 i = 0; i < file_count; i++) vfs::file_close(files[i]);
    kfree(files);
    return n;
}

i64 Socket::write(u64, const void* buffer, u64 length, u32 open_flags) {
    if (length == 0) return 0;
    u64 done = 0;
    while (done < length) {
        const u64 n = length - done < kSegmentMax ? length - done : kSegmentMax;
        auto* bounce = static_cast<u8*>(kmalloc(n));
        if (bounce == nullptr) return done ? static_cast<i64>(done) : -SIRIUS_ENOMEM;
        memcpy(bounce, static_cast<const u8*>(buffer) + done, n);
        const i64 sent = transmit(this, bounce, n, open_flags & SIRIUS_O_NONBLOCK, true, nullptr, 0);
        kfree(bounce);
        if (sent < 0) return done ? static_cast<i64>(done) : sent;
        done += static_cast<u64>(sent);
        if (static_cast<u64>(sent) < n) break;
    }
    return static_cast<i64>(done);
}

u32 Socket::poll(u64&) {
    ScopedLock guard(g_lock);
    u32 ready = 0;
    if (state == State::Listening) return accept_head ? SIRIUS_POLLIN : 0;
    if (state != State::Connected) return SIRIUS_POLLOUT | SIRIUS_POLLHUP;
    if (receive_bytes || peer_done || read_shut) ready |= SIRIUS_POLLIN;   // data, or end of stream to read
    if (peer == nullptr) ready |= SIRIUS_POLLHUP;
    else if (!write_shut && peer->receive_bytes < kReceiveCapacity) ready |= SIRIUS_POLLOUT;
    return ready;
}

// "sun_path" out of a user sockaddr_un, NUL-terminated whatever was passed.
i64 copy_address(char (&path)[sizeof(sirius_sockaddr_un::sun_path) + 1], u64 address, u64 length) {
    if (length < sizeof(u16) + 1 || length > sizeof(sirius_sockaddr_un)) return -SIRIUS_EINVAL;
    sirius_sockaddr_un name{};
    if (const int status = usercopy::copy_from(&name, address, length); status < 0) return status;
    if (name.sun_family != SIRIUS_AF_UNIX) return -SIRIUS_EAFNOSUPPORT;
    const u64 path_length = length - sizeof(u16);
    memcpy(path, name.sun_path, path_length);
    path[path_length] = '\0';
    // Linux's abstract names (a leading NUL) have no filesystem to live in.
    return path[0] ? 0 : -SIRIUS_EINVAL;
}

bool valid_type(u64 domain, u64 type, u64 protocol) {
    return domain == SIRIUS_AF_UNIX && protocol == 0 &&
           (type & ~static_cast<u64>(SIRIUS_SOCK_NONBLOCK | SIRIUS_SOCK_CLOEXEC)) == SIRIUS_SOCK_STREAM;
}

} // namespace

i64 sys_socket(u64 domain, u64 type, u64 protocol) {
    if (domain != SIRIUS_AF_UNIX) return -SIRIUS_EAFNOSUPPORT;
    if (!valid_type(domain, type, protocol)) return -SIRIUS_EINVAL;
    return install(new Socket(), SIRIUS_O_RDWR, type);
}

i64 sys_socketpair(u64 domain, u64 type, u64 protocol, u64 fds_address) {
    if (domain != SIRIUS_AF_UNIX) return -SIRIUS_EAFNOSUPPORT;
    if (!valid_type(domain, type, protocol)) return -SIRIUS_EINVAL;
    if (!usercopy::writable(fds_address, 2 * sizeof(i32))) return -SIRIUS_EFAULT;

    sched::Process* process = sched::current_process();
    auto* a = new Socket();
    auto* b = new Socket();
    {
        ScopedLock guard(g_lock);
        connect_pair(a, b, process->pid, process->pid);
    }
    const i64 first = install(a, SIRIUS_O_RDWR, type);
    const i64 second = install(b, SIRIUS_O_RDWR, type);
    if (first < 0 || second < 0) {
        const i64 made[2] = { first, second };
        for (const i64 fd : made) {
            if (fd < 0) continue;
            vfs::file_close(sched::detach_fd(process, static_cast<u64>(fd)));
        }
        return first < 0 ? first : second;
    }
    const i32 fds[2] = { static_cast<i32>(first), static_cast<i32>(second) };
    usercopy::copy_to(fds_address, fds, sizeof(fds));
    return 0;
}

i64 sys_bind(u64 fd, u64 address, u64 length) {
    Socket* self = socket_of(file_at(fd));
    if (self == nullptr) return file_at(fd) ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;
    char path[sizeof(sirius_sockaddr_un::sun_path) + 1];
    if (const i64 status = copy_address(path, address, length); status < 0) return status;
    {
        ScopedLock guard(g_lock);
        if (self->state != State::Unconnected || self->name) return -SIRIUS_EINVAL;
    }
    vfs::Vnode* node = nullptr;
    if (const int status = vfs::create_socket_node(sched::current_process()->cwd, path, node); status < 0) return status;
    ScopedLock guard(g_lock);
    self->name = node;
    return 0;
}

i64 sys_listen(u64 fd, u64 backlog) {
    Socket* self = socket_of(file_at(fd));
    if (self == nullptr) return file_at(fd) ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;
    ScopedLock guard(g_lock);
    if (self->state == State::Connected || self->name == nullptr) return -SIRIUS_EINVAL;
    self->backlog = backlog == 0 ? 1 : (backlog > kMaxBacklog ? kMaxBacklog : static_cast<u32>(backlog));
    if (self->state != State::Listening) {
        self->state = State::Listening;
        self->next_listener = g_listeners;
        g_listeners = self;
    }
    return 0;
}

// The connection exists from the moment connect() returns, and the client
// may write to it at once; accept() only gives the server its descriptor.
i64 sys_connect(u64 fd, u64 address, u64 length) {
    vfs::File* file = file_at(fd);
    Socket* self = socket_of(file);
    if (self == nullptr) return file ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;
    char path[sizeof(sirius_sockaddr_un::sun_path) + 1];
    if (const i64 status = copy_address(path, address, length); status < 0) return status;

    vfs::Vnode* node = nullptr;
    if (const int status = vfs::lookup(sched::current_process()->cwd, path, node); status < 0) return status;
    auto* server = new Socket();

    i64 result = 0;
    for (;;) {
        const u64 snapshot = events::snapshot();
        bool full = false;
        {
            ScopedLock guard(g_lock);
            Socket* listener = g_listeners;
            while (listener && listener->name != node) listener = listener->next_listener;
            if (self->state == State::Connected || self->was_connected) result = -SIRIUS_EISCONN;
            else if (self->state == State::Listening) result = -SIRIUS_EINVAL;
            else if (listener == nullptr) result = -SIRIUS_ECONNREFUSED;   // a name nobody listens behind
            else if (listener->pending >= listener->backlog) full = true;
            else {
                // The server's pid is not known until somebody accepts; the
                // listener's creator is not recorded, so report it then.
                connect_pair(self, server, sched::current_process()->pid, 0);
                if (listener->accept_tail) listener->accept_tail->next_pending = server;
                else listener->accept_head = server;
                listener->accept_tail = server;
                listener->pending++;
                server = nullptr;   // the queue owns it now
            }
        }
        if (!full) break;
        if (const i64 status = block(snapshot, file->flags & SIRIUS_O_NONBLOCK)) { result = status; break; }
    }
    node->unref();
    if (server) server->unref();
    if (result == 0) events::notify();
    return result;
}

i64 sys_accept(u64 fd, u64 flags) {
    vfs::File* file = file_at(fd);
    Socket* self = socket_of(file);
    if (self == nullptr) return file ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;
    if (flags & ~static_cast<u64>(SIRIUS_SOCK_NONBLOCK | SIRIUS_SOCK_CLOEXEC)) return -SIRIUS_EINVAL;

    for (;;) {
        const u64 snapshot = events::snapshot();
        Socket* connection = nullptr;
        {
            ScopedLock guard(g_lock);
            if (self->state != State::Listening) return -SIRIUS_EINVAL;
            connection = self->accept_head;
            if (connection) {
                self->accept_head = connection->next_pending;
                if (self->accept_head == nullptr) self->accept_tail = nullptr;
                connection->next_pending = nullptr;
                self->pending--;
                if (connection->peer) connection->peer->peer_pid = sched::current_process()->pid;
            }
        }
        if (connection) {
            events::notify();   // room in the backlog
            return install(connection, SIRIUS_O_RDWR, flags);
        }
        if (const i64 status = block(snapshot, file->flags & SIRIUS_O_NONBLOCK)) return status;
    }
}

i64 sys_shutdown(u64 fd, u64 how) {
    Socket* self = socket_of(file_at(fd));
    if (self == nullptr) return file_at(fd) ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;
    if (how > SIRIUS_SHUT_RDWR) return -SIRIUS_EINVAL;
    {
        ScopedLock guard(g_lock);
        if (self->state != State::Connected) return -SIRIUS_ENOTCONN;
        if (how != SIRIUS_SHUT_WR) self->read_shut = true;
        if (how != SIRIUS_SHUT_RD) {
            self->write_shut = true;
            if (self->peer) self->peer->peer_done = true;   // the peer reads to the end, then end of stream
        }
    }
    events::notify();
    return 0;
}

i64 sys_getsockopt(u64 fd, u64 level, u64 name, u64 value_address, u64 length_address) {
    Socket* self = socket_of(file_at(fd));
    if (self == nullptr) return file_at(fd) ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;
    if (level != SIRIUS_SOL_SOCKET) return -SIRIUS_EINVAL;
    u32 length;
    if (const int status = usercopy::copy_from(&length, length_address, sizeof(length)); status < 0) return status;

    if (name == SIRIUS_SO_PEERCRED) {
        sirius_ucred credentials{};   // everyone is uid 0: there are no users
        {
            ScopedLock guard(g_lock);
            if (!self->was_connected) return -SIRIUS_ENOTCONN;
            credentials.pid = static_cast<i32>(self->peer_pid);
        }
        if (length < sizeof(credentials)) return -SIRIUS_EINVAL;
        length = sizeof(credentials);
        if (const int status = usercopy::copy_to(value_address, &credentials, sizeof(credentials)); status < 0) return status;
    } else if (name == SIRIUS_SO_ERROR) {
        const i32 none = 0;
        if (length < sizeof(none)) return -SIRIUS_EINVAL;
        length = sizeof(none);
        if (const int status = usercopy::copy_to(value_address, &none, sizeof(none)); status < 0) return status;
    } else {
        return -SIRIUS_EOPNOTSUPP;
    }
    return usercopy::copy_to(length_address, &length, sizeof(length));
}

// --- sendmsg and recvmsg: data in pieces, and descriptors beside it -------------

namespace {

struct Message {
    sirius_msghdr header;
    sirius_iovec iov[kMaxIov];
    u64 total;   // bytes across the iovecs
};

i64 load_message(Message& message, u64 address, bool for_receive) {
    if (const int status = usercopy::copy_from(&message.header, address, sizeof(message.header)); status < 0) return status;
    if (message.header.iov_count > kMaxIov) return -SIRIUS_EINVAL;
    const u64 bytes = message.header.iov_count * sizeof(sirius_iovec);
    if (bytes) {
        if (const int status = usercopy::copy_from(message.iov, message.header.iov, bytes); status < 0) return status;
    }
    message.total = 0;
    for (u64 i = 0; i < message.header.iov_count; i++) {
        const sirius_iovec& piece = message.iov[i];
        if (piece.length > (1ULL << 30)) return -SIRIUS_EINVAL;
        if (!(for_receive ? usercopy::writable(piece.base, piece.length) : usercopy::readable(piece.base, piece.length)))
            return -SIRIUS_EFAULT;
        message.total += piece.length;
    }
    return 0;
}

// The files named by every SCM_RIGHTS record in a message's ancillary data,
// each with a reference taken.
i64 collect_files(const sirius_msghdr& header, vfs::File**& files, u32& count) {
    files = nullptr;
    count = 0;
    if (header.control == 0 || header.control_length < sizeof(sirius_cmsghdr)) return 0;
    if (header.control_length > 4096) return -SIRIUS_EINVAL;

    auto* control = static_cast<u8*>(kmalloc(header.control_length));
    if (control == nullptr) return -SIRIUS_ENOMEM;
    i64 status = usercopy::copy_from(control, header.control, header.control_length);

    files = static_cast<vfs::File**>(kzalloc(SIRIUS_SCM_MAX_FD * sizeof(vfs::File*)));
    if (files == nullptr) status = -SIRIUS_ENOMEM;

    for (u64 offset = 0; status == 0 && offset + sizeof(sirius_cmsghdr) <= header.control_length;) {
        sirius_cmsghdr record;
        memcpy(&record, control + offset, sizeof(record));
        if (record.length < sizeof(record) || record.length > header.control_length - offset) { status = -SIRIUS_EINVAL; break; }
        if (record.level == SIRIUS_SOL_SOCKET && record.type == SIRIUS_SCM_RIGHTS) {
            const u64 n = (record.length - sizeof(record)) / sizeof(i32);
            for (u64 i = 0; i < n; i++) {
                i32 fd;
                memcpy(&fd, control + offset + sizeof(record) + i * sizeof(i32), sizeof(fd));
                vfs::File* file = fd >= 0 ? file_at(static_cast<u64>(fd)) : nullptr;
                if (file == nullptr) { status = -SIRIUS_EBADF; break; }
                if (count == SIRIUS_SCM_MAX_FD) { status = -SIRIUS_EINVAL; break; }
                files[count++] = vfs::file_ref(file);
            }
        } else {
            status = -SIRIUS_EINVAL;   // no other kind of ancillary data exists here
        }
        offset += (static_cast<u64>(record.length) + 7) & ~7ULL;
    }
    kfree(control);

    if (status < 0 || count == 0) {
        for (u32 i = 0; i < count; i++) vfs::file_close(files[i]);
        kfree(files);
        files = nullptr;
        count = 0;
    }
    return status;
}

} // namespace

i64 sys_sendmsg(u64 fd, u64 message_address, u64 flags) {
    vfs::File* file = file_at(fd);
    Socket* self = socket_of(file);
    if (self == nullptr) return file ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;

    auto* message = static_cast<Message*>(kmalloc(sizeof(Message)));
    if (message == nullptr) return -SIRIUS_ENOMEM;
    i64 result = load_message(*message, message_address, false);

    vfs::File** files = nullptr;
    u32 file_count = 0;
    if (result == 0) result = collect_files(message->header, files, file_count);

    u8* data = nullptr;
    if (result == 0 && message->total) {
        data = static_cast<u8*>(kmalloc(message->total));
        if (data == nullptr) result = -SIRIUS_ENOMEM;
        for (u64 i = 0, at = 0; result == 0 && i < message->header.iov_count; at += message->iov[i].length, i++)
            memcpy(data + at, reinterpret_cast<const void*>(message->iov[i].base), message->iov[i].length);
    }

    if (result == 0) {
        const bool nonblocking = (file->flags & SIRIUS_O_NONBLOCK) || (flags & SIRIUS_MSG_DONTWAIT);
        result = transmit(self, data, message->total, nonblocking, !(flags & SIRIUS_MSG_NOSIGNAL), files, file_count);
    } else {
        for (u32 i = 0; i < file_count; i++) vfs::file_close(files[i]);
        kfree(files);
    }
    kfree(data);
    kfree(message);
    return result;
}

i64 sys_recvmsg(u64 fd, u64 message_address, u64 flags) {
    vfs::File* file = file_at(fd);
    Socket* self = socket_of(file);
    if (self == nullptr) return file ? -SIRIUS_ENOTSOCK : -SIRIUS_EBADF;

    auto* message = static_cast<Message*>(kmalloc(sizeof(Message)));
    if (message == nullptr) return -SIRIUS_ENOMEM;
    i64 result = load_message(*message, message_address, true);
    if (result == 0 && message->header.control && !usercopy::writable(message->header.control, message->header.control_length))
        result = -SIRIUS_EFAULT;
    if (result < 0) { kfree(message); return result; }

    const u64 capacity = message->total < 4 * kSegmentMax ? message->total : 4 * kSegmentMax;
    auto* data = static_cast<u8*>(kmalloc(capacity ? capacity : 1));
    if (data == nullptr) { kfree(message); return -SIRIUS_ENOMEM; }

    vfs::File** files = nullptr;
    u32 file_count = 0;
    const bool nonblocking = (file->flags & SIRIUS_O_NONBLOCK) || (flags & SIRIUS_MSG_DONTWAIT);
    result = capacity ? receive(self, data, capacity, nonblocking, files, file_count) : 0;

    if (result > 0) {
        u64 at = 0;
        for (u64 i = 0; i < message->header.iov_count && at < static_cast<u64>(result); i++) {
            u64 n = message->iov[i].length;
            if (n > static_cast<u64>(result) - at) n = static_cast<u64>(result) - at;
            memcpy(reinterpret_cast<void*>(message->iov[i].base), data + at, n);
            at += n;
        }
    }

    // The files become descriptors here, in the receiver, in the order they
    // were sent -- as many as the control buffer has room to name. The rest
    // are closed and the message marked truncated.
    u64 control_used = 0;
    i32 out_flags = 0;
    if (file_count) {
        const u64 room = message->header.control_length >= sizeof(sirius_cmsghdr)
                       ? (message->header.control_length - sizeof(sirius_cmsghdr)) / sizeof(i32) : 0;
        sched::Process* process = sched::current_process();
        u32 delivered = 0;
        i32 fds[SIRIUS_SCM_MAX_FD];
        for (; delivered < file_count && delivered < room; delivered++) {
            const i64 new_fd = sched::install_fd(process, files[delivered],
                                                 (flags & SIRIUS_MSG_CMSG_CLOEXEC) ? SIRIUS_FD_CLOEXEC : 0);
            if (new_fd < 0) break;   // the table is full
            fds[delivered] = static_cast<i32>(new_fd);
        }
        for (u32 i = delivered; i < file_count; i++) vfs::file_close(files[i]);
        if (delivered < file_count) out_flags |= SIRIUS_MSG_CTRUNC;
        if (delivered) {
            const sirius_cmsghdr record{ static_cast<u32>(sizeof(sirius_cmsghdr) + delivered * sizeof(i32)), 0, SIRIUS_SOL_SOCKET, SIRIUS_SCM_RIGHTS };
            memcpy(reinterpret_cast<void*>(message->header.control), &record, sizeof(record));
            memcpy(reinterpret_cast<void*>(message->header.control + sizeof(record)), fds, delivered * sizeof(i32));
            control_used = record.length;
        }
        kfree(files);
    }

    if (result >= 0) {
        message->header.control_length = control_used;
        message->header.flags = out_flags;
        message->header.name_length = 0;
        usercopy::copy_to(message_address, &message->header, sizeof(message->header));
    }
    kfree(data);
    kfree(message);
    return result;
}

} // namespace ipc
