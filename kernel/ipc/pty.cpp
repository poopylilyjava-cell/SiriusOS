// Pseudo-terminals: two descriptors with a line discipline between them.
//
// The master is held by a terminal emulator. What it writes is what the user
// typed: it passes through the line discipline -- echoed, edited a line at a
// time, Ctrl+C turned into SIGINT -- and the program on the slave reads the
// result. What that program writes comes out of the master, with newlines
// made into carriage return and line feed for the emulator to draw.
//
// The discipline is the part of POSIX's general terminal interface that
// programs on a pseudo-terminal actually meet: canonical and raw input,
// echo, erase, kill, end of file, the three signal characters, carriage
// return and newline translation, VMIN and VTIME, a window size and a
// foreground process group. Not there: flow control (^S/^Q), parity and
// baud rates (kept, and meaningless), job control's SIGTTIN and SIGTTOU.
//
// Like the pipes, a pair has no name: openpty() makes both descriptors at
// once (SYS_OPENPTY), and /dev/ptmx and /dev/pts do not exist.
#include <ipc/ipc.hpp>
#include <arch/x86_64/clock.hpp>
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
#include <sirius/signal.h>
#include <sirius/tty.h>

namespace ipc {
namespace {

constexpr u64 kInputCapacity = 4096;     // typed and not yet read
constexpr u64 kOutputCapacity = 16384;   // written and not yet shown
constexpr u64 kLineMax = 1024;           // the longest line canonical mode edits
constexpr u64 kChunk = 1024;             // what crosses between user memory and a pty at a time

struct Ring {
    u8* data;
    u64 capacity, head, count;

    u64 space() const { return capacity - count; }
    bool push(u8 c) {
        if (count == capacity) return false;
        data[(head + count++) % capacity] = c;
        return true;
    }
    u8 at(u64 i) const { return data[(head + i) % capacity]; }
    u64 pop(u8* out, u64 n) {
        if (n > count) n = count;
        for (u64 i = 0; i < n; i++) out[i] = at(i);
        head = (head + n) % capacity;
        count -= n;
        return n;
    }
    void clear() { head = count = 0; }
};

struct Pty {
    Ring input;                 // what the slave reads: in canonical mode, finished lines only
    Ring output;                // what the master reads
    u8   line[kLineMax];        // canonical mode: the line being typed
    u64  line_length;
    u32  eofs;                  // Ctrl+D at the start of a line: each is one read that returns 0
    sirius_termios termios;
    sirius_winsize winsize;
    u32  session;               // the session it is the controlling terminal of; 0 for none
    u32  foreground;            // the process group that gets its signals
    bool master_open, slave_open;
    u32  ends;                  // 2, 1, then 0 and freed
};

bool has(const Pty* pty, u32 lflag) { return pty->termios.c_lflag & lflag; }

// Program output, as the master will read it.
void emit(Pty* pty, u8 c) {
    if ((pty->termios.c_oflag & SIRIUS_OPOST) && (pty->termios.c_oflag & SIRIUS_ONLCR) && c == '\n')
        pty->output.push('\r');
    pty->output.push(c);
}

// Echo of something typed: control characters as ^X if asked for.
void echo(Pty* pty, u8 c) {
    if (has(pty, SIRIUS_ECHOCTL) && (c < 32 || c == 127) && c != '\n' && c != '\t' && c != '\r') {
        emit(pty, '^');
        emit(pty, c == 127 ? '?' : static_cast<u8>(c + 64));
        return;
    }
    emit(pty, c);
}

// Rub out the last character of the line, on screen too.
bool erase_one(Pty* pty) {
    if (pty->line_length == 0) return false;
    pty->line_length--;
    if (has(pty, SIRIUS_ECHO) && has(pty, SIRIUS_ECHOE)) {
        emit(pty, '\b');
        emit(pty, ' ');
        emit(pty, '\b');
    }
    return true;
}

void finish_line(Pty* pty) {
    for (u64 i = 0; i < pty->line_length; i++) pty->input.push(pty->line[i]);
    pty->line_length = 0;
}

// One character the user typed. Returns a signal to send the foreground
// process group, or 0.
int discipline(Pty* pty, u8 c) {
    const sirius_termios& t = pty->termios;
    if (c == '\r' && (t.c_iflag & SIRIUS_IGNCR)) return 0;
    if (c == '\r' && (t.c_iflag & SIRIUS_ICRNL)) c = '\n';
    else if (c == '\n' && (t.c_iflag & SIRIUS_INLCR)) c = '\r';

    if (has(pty, SIRIUS_ISIG)) {
        int sig = 0;
        if (c == t.c_cc[SIRIUS_VINTR]) sig = SIRIUS_SIGINT;
        else if (c == t.c_cc[SIRIUS_VQUIT]) sig = SIRIUS_SIGQUIT;
        else if (c == t.c_cc[SIRIUS_VSUSP]) sig = SIRIUS_SIGTSTP;
        if (sig != 0 && c != 0) {
            // What was typed is abandoned along with what is running.
            pty->line_length = 0;
            pty->input.clear();
            if (has(pty, SIRIUS_ECHO)) echo(pty, c);
            return sig;
        }
    }

    if (has(pty, SIRIUS_ICANON)) {
        if (c == t.c_cc[SIRIUS_VERASE] && c != 0) { erase_one(pty); return 0; }
        if (c == t.c_cc[SIRIUS_VKILL] && c != 0) { while (erase_one(pty)) {} return 0; }
        if (c == t.c_cc[SIRIUS_VWERASE] && c != 0 && has(pty, SIRIUS_IEXTEN)) {
            while (pty->line_length && pty->line[pty->line_length - 1] == ' ') erase_one(pty);
            while (pty->line_length && pty->line[pty->line_length - 1] != ' ') erase_one(pty);
            return 0;
        }
        if (c == t.c_cc[SIRIUS_VEOF] && c != 0) {
            // A line typed so far is sent as it is; on its own, end of file.
            if (pty->line_length == 0) pty->eofs++;
            else finish_line(pty);
            return 0;
        }
        const bool end = c == '\n' || (c == t.c_cc[SIRIUS_VEOL] && c != 0);
        // The last place is kept for the end of the line.
        if (pty->line_length < (end ? kLineMax : kLineMax - 1))
            pty->line[pty->line_length++] = c;
        if (has(pty, SIRIUS_ECHO) || (c == '\n' && has(pty, SIRIUS_ECHONL))) echo(pty, c);
        // A line that would not fit in what the reader has not taken yet
        // waits in the line buffer, and further typing is lost.
        if (end && pty->input.space() >= pty->line_length) finish_line(pty);
        return 0;
    }

    pty->input.push(c);
    if (has(pty, SIRIUS_ECHO)) echo(pty, c);
    return 0;
}

// How much a read on the slave may take now: in canonical mode, up to the
// end of the first line.
u64 readable(const Pty* pty, u64 wanted) {
    u64 n = pty->input.count < wanted ? pty->input.count : wanted;
    if (has(pty, SIRIUS_ICANON))
        for (u64 i = 0; i < n; i++)
            if (pty->input.at(i) == '\n') return i + 1;
    return n;
}

void send_to_group(u32 group, int sig) {
    if (group != 0 && sig != 0) sched::signal_group(group, sig);
}

class PtyEnd final : public vfs::Vnode {
public:
    PtyEnd(Pty* pty, bool master) : Vnode(vfs::anonymous_fs(), vfs::NodeType::CharDevice, 0), m_pty(pty), m_master(master) {}

    ~PtyEnd() override {
        bool last;
        u32 hangup = 0;
        {
            ScopedLock guard(g_lock);
            if (m_master) {
                m_pty->master_open = false;
                hangup = m_pty->session;   // the terminal is gone: its session is told
            } else {
                m_pty->slave_open = false;
            }
            last = --m_pty->ends == 0;
        }
        if (hangup) send_to_group(hangup, SIRIUS_SIGHUP);
        if (last) {
            kfree(m_pty->input.data);
            kfree(m_pty->output.data);
            kfree(m_pty);
        }
        events::notify();
    }

    i64 read(u64, void* buffer, u64 length, u32 open_flags) override {
        if (length == 0) return 0;
        for (;;) {
            const u64 snapshot = events::snapshot();
            u8 chunk[kChunk];
            u64 n = 0;
            i64 status = 0;
            u64 deadline = 0;
            bool wait = true;
            {
                ScopedLock guard(g_lock);
                const u64 want = length < sizeof(chunk) ? length : sizeof(chunk);
                if (m_master) {
                    n = m_pty->output.pop(chunk, want);
                    if (n == 0 && !m_pty->slave_open) status = -SIRIUS_EIO;   // as Linux: the other side has gone
                } else {
                    n = m_pty->input.pop(chunk, readable(m_pty, want));
                    if (n == 0) {
                        const u8 vmin = m_pty->termios.c_cc[SIRIUS_VMIN], vtime = m_pty->termios.c_cc[SIRIUS_VTIME];
                        if (has(m_pty, SIRIUS_ICANON) && m_pty->eofs) { m_pty->eofs--; wait = false; }
                        else if (!m_pty->master_open) wait = false;                      // hung up: end of file
                        else if (!has(m_pty, SIRIUS_ICANON) && vmin == 0 && vtime == 0) wait = false;
                        else if (!has(m_pty, SIRIUS_ICANON) && vmin == 0)
                            deadline = clock::now_ns() + static_cast<u64>(vtime) * 100000000ull;
                    }
                }
            }
            if (n) {
                memcpy(buffer, chunk, n);
                events::notify();   // there is room again
                return static_cast<i64>(n);
            }
            if (status) return status;
            if (!wait) return 0;
            if (deadline && clock::now_ns() >= deadline) return 0;
            if (const i64 blocked = block(snapshot, open_flags & SIRIUS_O_NONBLOCK, deadline)) return blocked;
            if (deadline && clock::now_ns() >= deadline) return 0;
        }
    }

    i64 write(u64, const void* buffer, u64 length, u32 open_flags) override {
        const auto* in = static_cast<const u8*>(buffer);
        u64 done = 0;
        while (done < length) {
            const u64 snapshot = events::snapshot();
            u8 chunk[kChunk];
            const u64 want = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
            memcpy(chunk, in + done, want);

            u64 n = 0;
            int sig = 0;
            u32 group = 0;
            bool hung_up = false;
            {
                ScopedLock guard(g_lock);
                if (m_master) {
                    // Typing: never blocks. What does not fit is lost, as
                    // with a real keyboard nobody is reading.
                    for (; n < want; n++)
                        if (const int raised = discipline(m_pty, chunk[n]); raised) sig = raised;
                    group = m_pty->foreground;
                } else if (!m_pty->master_open) {
                    hung_up = true;
                } else {
                    // Output: as much as fits, counting the carriage returns
                    // ONLCR adds.
                    while (n < want && m_pty->output.space() >= 2) emit(m_pty, chunk[n++]);
                }
            }
            if (hung_up) return done ? static_cast<i64>(done) : -SIRIUS_EIO;
            if (sig) send_to_group(group, sig);
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
        if (m_master) {
            if (m_pty->output.count) ready |= SIRIUS_POLLIN;
            if (!m_pty->slave_open) ready |= SIRIUS_POLLHUP;
            ready |= SIRIUS_POLLOUT;
        } else {
            if (readable(m_pty, kInputCapacity) || (has(m_pty, SIRIUS_ICANON) && m_pty->eofs)) ready |= SIRIUS_POLLIN;
            if (!m_pty->master_open) ready |= SIRIUS_POLLIN | SIRIUS_POLLHUP;
            if (m_pty->output.space() >= 2) ready |= SIRIUS_POLLOUT;
        }
        return ready;
    }

    int ioctl(u64 request, u64 argument) override {
        sched::Process* process = sched::current_process();
        switch (request) {
        case SIRIUS_TCGETS: {
            sirius_termios termios;
            { ScopedLock guard(g_lock); termios = m_pty->termios; }
            return usercopy::copy_to(argument, &termios, sizeof(termios));
        }
        case SIRIUS_TCSETS: case SIRIUS_TCSETSW: case SIRIUS_TCSETSF: {
            sirius_termios termios;
            if (const int status = usercopy::copy_from(&termios, argument, sizeof(termios)); status < 0) return status;
            {
                ScopedLock guard(g_lock);
                // Leaving canonical mode hands over the line typed so far.
                if ((m_pty->termios.c_lflag & SIRIUS_ICANON) && !(termios.c_lflag & SIRIUS_ICANON)) finish_line(m_pty);
                m_pty->termios = termios;
                if (request == SIRIUS_TCSETSF) { m_pty->input.clear(); m_pty->line_length = 0; }
            }
            events::notify();
            return 0;
        }
        case SIRIUS_TCFLSH: {
            ScopedLock guard(g_lock);
            if (argument == 0 || argument == 2) { m_pty->input.clear(); m_pty->line_length = 0; m_pty->eofs = 0; }
            if (argument == 1 || argument == 2) m_pty->output.clear();
            return argument <= 2 ? 0 : -SIRIUS_EINVAL;
        }
        case SIRIUS_TIOCGWINSZ: {
            sirius_winsize size;
            { ScopedLock guard(g_lock); size = m_pty->winsize; }
            return usercopy::copy_to(argument, &size, sizeof(size));
        }
        case SIRIUS_TIOCSWINSZ: {
            sirius_winsize size;
            if (const int status = usercopy::copy_from(&size, argument, sizeof(size)); status < 0) return status;
            u32 group = 0;
            {
                ScopedLock guard(g_lock);
                if (memcmp(&size, &m_pty->winsize, sizeof(size)) != 0) group = m_pty->foreground;
                m_pty->winsize = size;
            }
            send_to_group(group, SIRIUS_SIGWINCH);
            return 0;
        }
        case SIRIUS_TIOCSCTTY: {
            // Only a session leader may acquire one, and only a terminal no
            // other session has.
            if (m_master || process->sid != process->pid) return -SIRIUS_EPERM;
            ScopedLock guard(g_lock);
            if (m_pty->session != 0 && m_pty->session != process->sid) return -SIRIUS_EPERM;
            m_pty->session = process->sid;
            m_pty->foreground = process->pgid;
            return 0;
        }
        case SIRIUS_TIOCNOTTY: {
            ScopedLock guard(g_lock);
            if (m_pty->session != process->sid) return -SIRIUS_ENOTTY;
            m_pty->session = m_pty->foreground = 0;
            return 0;
        }
        case SIRIUS_TIOCGPGRP: case SIRIUS_TIOCGSID: {
            i32 value;
            {
                ScopedLock guard(g_lock);
                if (m_pty->session == 0) return -SIRIUS_ENOTTY;
                value = static_cast<i32>(request == SIRIUS_TIOCGPGRP ? m_pty->foreground : m_pty->session);
            }
            return usercopy::copy_to(argument, &value, sizeof(value));
        }
        case SIRIUS_TIOCSPGRP: {
            i32 group;
            if (const int status = usercopy::copy_from(&group, argument, sizeof(group)); status < 0) return status;
            if (group <= 0) return -SIRIUS_EINVAL;
            ScopedLock guard(g_lock);
            if (m_pty->session == 0 || m_pty->session != process->sid) return -SIRIUS_ENOTTY;
            m_pty->foreground = static_cast<u32>(group);
            return 0;
        }
        case SIRIUS_FIONREAD: {
            i32 count;
            { ScopedLock guard(g_lock); count = static_cast<i32>(m_master ? m_pty->output.count : m_pty->input.count); }
            return usercopy::copy_to(argument, &count, sizeof(count));
        }
        default:
            return -SIRIUS_ENOTTY;
        }
    }

private:
    Pty* m_pty;
    bool m_master;
};

} // namespace

i64 sys_openpty(u64 fds_address, u64 termios_address, u64 winsize_address) {
    if (!usercopy::writable(fds_address, 2 * sizeof(i32))) return -SIRIUS_EFAULT;

    auto* pty = static_cast<Pty*>(kzalloc(sizeof(Pty)));
    if (pty == nullptr) return -SIRIUS_ENOMEM;
    pty->input = { static_cast<u8*>(kmalloc(kInputCapacity)), kInputCapacity, 0, 0 };
    pty->output = { static_cast<u8*>(kmalloc(kOutputCapacity)), kOutputCapacity, 0, 0 };
    if (pty->input.data == nullptr || pty->output.data == nullptr) {
        kfree(pty->input.data);
        kfree(pty->output.data);
        kfree(pty);
        return -SIRIUS_ENOMEM;
    }
    // What a Linux pseudo-terminal starts with.
    sirius_termios& t = pty->termios;
    t.c_iflag = SIRIUS_ICRNL;
    t.c_oflag = SIRIUS_OPOST | SIRIUS_ONLCR;
    t.c_cflag = SIRIUS_B38400 | SIRIUS_CS8 | SIRIUS_CREAD;
    t.c_lflag = SIRIUS_ISIG | SIRIUS_ICANON | SIRIUS_ECHO | SIRIUS_ECHOE | SIRIUS_ECHOK | SIRIUS_ECHOCTL | SIRIUS_IEXTEN;
    t.c_cc[SIRIUS_VINTR] = 3;       // Ctrl+C
    t.c_cc[SIRIUS_VQUIT] = 28;      // Ctrl+backslash
    t.c_cc[SIRIUS_VERASE] = 127;    // the Backspace key sends DEL
    t.c_cc[SIRIUS_VKILL] = 21;      // Ctrl+U
    t.c_cc[SIRIUS_VEOF] = 4;        // Ctrl+D
    t.c_cc[SIRIUS_VMIN] = 1;
    t.c_cc[SIRIUS_VSUSP] = 26;      // Ctrl+Z
    t.c_cc[SIRIUS_VWERASE] = 23;    // Ctrl+W
    t.c_ispeed = t.c_ospeed = 38400;
    pty->winsize = { 24, 80, 0, 0 };
    if (termios_address && usercopy::copy_from(&pty->termios, termios_address, sizeof(pty->termios)) < 0) {
        kfree(pty->input.data); kfree(pty->output.data); kfree(pty);
        return -SIRIUS_EFAULT;
    }
    if (winsize_address && usercopy::copy_from(&pty->winsize, winsize_address, sizeof(pty->winsize)) < 0) {
        kfree(pty->input.data); kfree(pty->output.data); kfree(pty);
        return -SIRIUS_EFAULT;
    }
    pty->master_open = pty->slave_open = true;
    pty->ends = 2;

    const i64 master = install(new PtyEnd(pty, true), SIRIUS_O_RDWR, 0);
    const i64 slave = install(new PtyEnd(pty, false), SIRIUS_O_RDWR, 0);
    if (master < 0 || slave < 0) {
        // As for pipes: an end that got a descriptor is closed through it;
        // one that did not was released by install().
        sched::Process* process = sched::current_process();
        const i64 made[2] = { master, slave };
        for (const i64 fd : made)
            if (fd >= 0) vfs::file_close(sched::detach_fd(process, static_cast<u64>(fd)));
        return master < 0 ? master : slave;
    }
    const i32 fds[2] = { static_cast<i32>(master), static_cast<i32>(slave) };
    usercopy::copy_to(fds_address, fds, sizeof(fds));
    return 0;
}

} // namespace ipc
