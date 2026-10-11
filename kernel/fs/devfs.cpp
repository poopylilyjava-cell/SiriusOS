#include <fs/devfs.hpp>
#include <arch/x86_64/serial.hpp>
#include <drivers/fbconsole.hpp>
#include <klib/spinlock.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sched/process.hpp>
#include <sirius/errno.h>

namespace devfs {
namespace {

using vfs::NodeType;
using vfs::Vnode;

constexpr u32 kMaxDevices = 32;

class DevFs;
DevFs* g_instance = nullptr;

// /dev/console: the serial port, until there is a keyboard and a terminal.
// Reads report end-of-file: there is no input device yet.
class Console final : public Vnode {
public:
    explicit Console(vfs::Filesystem* fs) : Vnode(fs, NodeType::CharDevice, 1) {}
    i64 read(u64, void*, u64) override { return 0; }
    i64 write(u64, const void* buffer, u64 length) override {
        // serial_write takes a C string; chunks keep a NUL in a user buffer
        // from ending the write early, and each chunk goes out atomically.
        const auto* bytes = static_cast<const char*>(buffer);
        char chunk[256];
        for (u64 done = 0; done < length;) {
            u64 n = 0;
            while (n < sizeof(chunk) - 1 && done + n < length) {
                const char c = bytes[done + n];
                chunk[n++] = c ? c : ' ';
            }
            chunk[n] = '\0';
            serial_write(chunk);
            fbconsole::write(chunk);
            done += n;
        }
        return static_cast<i64>(length);
    }
};

class Null final : public Vnode {
public:
    explicit Null(vfs::Filesystem* fs) : Vnode(fs, NodeType::CharDevice, 2) {}
    i64 read(u64, void*, u64) override { return 0; }
    i64 write(u64, const void*, u64 length) override { return static_cast<i64>(length); }
};

class Zero final : public Vnode {
public:
    explicit Zero(vfs::Filesystem* fs) : Vnode(fs, NodeType::CharDevice, 3) {}
    i64 read(u64, void* buffer, u64 length) override {
        memset(buffer, 0, length);
        return static_cast<i64>(length);
    }
    i64 write(u64, const void*, u64 length) override { return static_cast<i64>(length); }
};

// /dev/fd: a directory with one entry per open descriptor of whichever
// process reads it. It can only be listed -- which is what programs do with
// it, to count their descriptors or close all but a few; the entries cannot
// be opened.
class FdDirectory final : public Vnode {
public:
    explicit FdDirectory(vfs::Filesystem* fs) : Vnode(fs, NodeType::Directory, 6) {}

    int lookup(const char*, Vnode*&) override { return -SIRIUS_ENOENT; }
    int create(const char*, NodeType, Vnode*&) override { return -SIRIUS_EACCES; }
    int unlink(const char*, Vnode*) override { return -SIRIUS_EACCES; }

    // The cookie is the next descriptor number to look at.
    int readdir(u64& cookie, vfs::DirEntry& out) override {
        const sched::Process* process = sched::current_process();
        if (process == nullptr) return 0;
        while (cookie < sched::kMaxFiles && process->files[cookie] == nullptr) cookie++;
        if (cookie >= sched::kMaxFiles) return 0;
        ksnprintf(out.name, sizeof(out.name), "%lu", cookie);
        out.type = NodeType::CharDevice;
        out.inode = 100 + cookie;
        cookie++;
        return 1;
    }
};

struct Entry {
    char   name[32];
    Vnode* node;
};

// The root directory: a fixed table. Devices are added at boot, never removed.
class Root final : public Vnode {
public:
    explicit Root(vfs::Filesystem* fs) : Vnode(fs, NodeType::Directory, 0) {}

    bool add(const char* name, Vnode* node) {
        ScopedLock guard(m_lock);
        if (m_count == kMaxDevices) return false;
        for (u32 i = 0; i < m_count; i++) if (strcmp(m_entries[i].name, name) == 0) return false;
        str_copy(m_entries[m_count].name, sizeof(m_entries[0].name), name);
        m_entries[m_count].node = node;
        __atomic_store_n(&m_count, m_count + 1, __ATOMIC_RELEASE);
        return true;
    }

    int lookup(const char* name, Vnode*& out) override {
        const u32 count = __atomic_load_n(&m_count, __ATOMIC_ACQUIRE);
        for (u32 i = 0; i < count; i++) {
            if (strcmp(m_entries[i].name, name) == 0) {
                m_entries[i].node->ref();
                out = m_entries[i].node;
                return 0;
            }
        }
        return -SIRIUS_ENOENT;
    }

    int create(const char*, NodeType, Vnode*&) override { return -SIRIUS_EACCES; }
    int unlink(const char*, Vnode*) override { return -SIRIUS_EACCES; }

    int readdir(u64& cookie, vfs::DirEntry& out) override {
        if (cookie >= __atomic_load_n(&m_count, __ATOMIC_ACQUIRE)) return 0;
        const Entry& entry = m_entries[cookie++];
        str_copy(out.name, sizeof(out.name), entry.name);
        out.type = entry.node->type();
        out.inode = entry.node->inode();
        return 1;
    }

private:
    Spinlock m_lock;
    Entry m_entries[kMaxDevices] = {};
    u32 m_count = 0;
};

class DevFs final : public vfs::Filesystem {
public:
    DevFs() : m_root(new Root(this)) {
        m_root->add("console", new Console(this));
        m_root->add("null", new Null(this));
        m_root->add("zero", new Zero(this));
        m_root->add("fd", new FdDirectory(this));
    }
    const char* name() const override { return "devfs"; }
    Vnode* root() override { m_root->ref(); return m_root; }
    // Devices synchronise themselves; none of them may wait behind the disk.
    bool serialized() const override { return false; }
    bool add(const char* name, Vnode* node) { return m_root->add(name, node); }

private:
    Root* m_root;
};

} // namespace

vfs::Filesystem* instance() {
    if (g_instance == nullptr) g_instance = new DevFs();
    return g_instance;
}

bool add(const char* name, vfs::Vnode* node) {
    return static_cast<DevFs*>(instance())->add(name, node);
}

} // namespace devfs
