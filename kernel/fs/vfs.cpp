#include <fs/vfs.hpp>
#include <sched/mutex.hpp>
#include <sched/events.hpp>
#include <klib/spinlock.hpp>
#include <mm/heap.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>
#include <sirius/poll.h>
#include <ipc/epoll.hpp>

namespace vfs {

struct Dentry {
    char    name[path::kNameMax + 1];
    Dentry* parent;     // null for the global root and for mounted roots
    Dentry* covers;     // a mounted filesystem's root: the dentry it is mounted on
    Dentry* mounted;    // the root of whatever is mounted here, if anything
    Dentry* children;   // cached children; there are no negative entries
    Dentry* sibling;
    Vnode*  vnode;      // one reference, held for the dentry's lifetime
    u32     refs;       // references from outside the tree: cwds, open files
    bool    unlinked;
};

namespace {

sched::Mutex g_lock;
Dentry* g_root = nullptr;
u32 g_next_device = 1;

class Lock {
public:
    explicit Lock(bool take = true) : m_held(take) { if (m_held) g_lock.lock(); }
    ~Lock() { if (m_held) g_lock.unlock(); }
private:
    bool m_held;
};

int error(int code) { return -code; }

Dentry* new_dentry(Dentry* parent, const char* name, Vnode* vnode) {
    auto* dentry = static_cast<Dentry*>(kzalloc(sizeof(Dentry)));
    if (dentry == nullptr) return nullptr;
    str_copy(dentry->name, sizeof(dentry->name), name);
    dentry->parent = parent;
    dentry->vnode = vnode;
    if (parent) {
        dentry->sibling = parent->children;
        parent->children = dentry;
    }
    return dentry;
}

void detach(Dentry* dentry) {
    Dentry* parent = dentry->parent;
    if (parent == nullptr) return;
    for (Dentry** link = &parent->children; *link; link = &(*link)->sibling) {
        if (*link == dentry) { *link = dentry->sibling; break; }
    }
    dentry->sibling = nullptr;
}

void free_if_dead(Dentry* dentry) {
    if (!dentry->unlinked || dentry->refs != 0) return;
    dentry->vnode->unref();
    kfree(dentry);
}

// Step over any filesystems stacked on this dentry.
Dentry* follow_mounts(Dentry* dentry) {
    while (dentry->mounted) dentry = dentry->mounted;
    return dentry;
}

Dentry* parent_of(Dentry* dentry) {
    if (dentry == g_root) return g_root;
    // A mounted filesystem's root leads back out through its mount point.
    while (dentry->covers) dentry = dentry->covers;
    return dentry->parent ? dentry->parent : g_root;
}

// The child `name` of directory `dir`, from the cache or else the filesystem.
int child(Dentry* dir, const char* name, Dentry*& out) {
    if (!dir->vnode->is_directory()) return error(SIRIUS_ENOTDIR);

    if (strcmp(name, "..") == 0) {
        out = follow_mounts(parent_of(dir));
        return 0;
    }

    for (Dentry* d = dir->children; d; d = d->sibling) {
        if (strcmp(d->name, name) == 0) {
            out = follow_mounts(d);
            return 0;
        }
    }

    Vnode* vnode = nullptr;
    const int result = dir->vnode->lookup(name, vnode);
    if (result < 0) return result;

    Dentry* dentry = new_dentry(dir, name, vnode);
    if (dentry == nullptr) { vnode->unref(); return error(SIRIUS_ENOMEM); }
    out = follow_mounts(dentry);
    return 0;
}

enum class Stop { AtEnd, BeforeLast };

// Walk `path` from `cwd` (or the root). With Stop::BeforeLast, `out` is the
// directory that would contain the final component, which is copied to
// `leaf` -- empty if the path names the root itself.
int resolve(Dentry* cwd, const char* path, Stop stop, Dentry*& out, char* leaf = nullptr) {
    if (g_root == nullptr) return error(SIRIUS_ENOENT);
    if (path[0] == '\0') return error(SIRIUS_ENOENT);

    path::Components components(path);
    Dentry* current = follow_mounts((components.absolute() || cwd == nullptr) ? g_root : cwd);
    if (current->unlinked) return error(SIRIUS_ENOENT);
    if (leaf) leaf[0] = '\0';

    char name[path::kNameMax + 1];
    for (;;) {
        const auto result = components.next(name);
        if (result == path::Components::Result::End) break;
        if (result == path::Components::Result::NameTooLong) return error(SIRIUS_ENAMETOOLONG);

        if (stop == Stop::BeforeLast && components.at_end()) {
            if (!current->vnode->is_directory()) return error(SIRIUS_ENOTDIR);
            str_copy(leaf, path::kNameMax + 1, name);
            break;
        }

        const int status = child(current, name, current);
        if (status < 0) return status;
    }

    out = current;
    return 0;
}

u32 mode_for(NodeType type) {
    switch (type) {
    case NodeType::Directory:   return SIRIUS_S_IFDIR | 0755;
    case NodeType::CharDevice:  return SIRIUS_S_IFCHR | 0666;
    case NodeType::BlockDevice: return SIRIUS_S_IFBLK | 0660;
    case NodeType::Fifo:        return SIRIUS_S_IFIFO | 0600;
    case NodeType::Socket:      return SIRIUS_S_IFSOCK | 0777;
    case NodeType::Regular:     break;
    }
    return SIRIUS_S_IFREG | 0644;
}

void fill_stat(Vnode* vnode, sirius_stat& out) {
    out = {};
    out.st_ino     = vnode->inode();
    out.st_size    = vnode->size();
    out.st_blocks  = (out.st_size + 511) / 512;
    out.st_mode    = mode_for(vnode->type());
    out.st_nlink   = 1;
    out.st_blksize = 4096;
    out.st_dev     = vnode->fs()->device_id;
}

bool wants_write(u32 flags) { return (flags & SIRIUS_O_ACCMODE) != SIRIUS_O_RDONLY; }
bool can_read(u32 flags)    { return (flags & SIRIUS_O_ACCMODE) != SIRIUS_O_WRONLY; }

} // namespace

// --- Vnode defaults: a directory-only or file-only filesystem overrides half.
int Vnode::lookup(const char*, Vnode*&)          { return error(SIRIUS_ENOTDIR); }
int Vnode::create(const char*, NodeType, Vnode*&) { return error(SIRIUS_ENOTDIR); }
int Vnode::unlink(const char*, Vnode*)           { return error(SIRIUS_ENOTDIR); }
int Vnode::readdir(u64&, DirEntry&)              { return error(SIRIUS_ENOTDIR); }
i64 Vnode::read(u64, void*, u64)                 { return error(SIRIUS_EINVAL); }
i64 Vnode::read(u64 offset, void* buffer, u64 length, u32) { return read(offset, buffer, length); }
i64 Vnode::write(u64, const void*, u64)          { return error(SIRIUS_EINVAL); }
i64 Vnode::write(u64 offset, const void* buffer, u64 length, u32) { return write(offset, buffer, length); }
u32 Vnode::poll(u64&)                            { return SIRIUS_POLLIN | SIRIUS_POLLOUT; }
i64 Vnode::shared_page(u64)                      { return error(SIRIUS_ENODEV); }
int Vnode::ioctl(u64, u64)                       { return error(SIRIUS_ENOTTY); }
i64 Vnode::physical_page(u64, bool&)             { return error(SIRIUS_ENODEV); }
int Vnode::truncate(u64)                         { return error(SIRIUS_EINVAL); }

void Vnode::unref() {
    if (__atomic_sub_fetch(&m_refs, 1, __ATOMIC_ACQ_REL) == 0) delete this;
}

// --- mounting -----------------------------------------------------------------

extern Filesystem* g_anonymous;
Filesystem* make_anonymous_fs();

int mount_root(Filesystem* fs) {
    Lock lock;
    if (g_root != nullptr) return error(SIRIUS_EBUSY);
    if (g_anonymous == nullptr) g_anonymous = make_anonymous_fs();
    Dentry* root = new_dentry(nullptr, "/", fs->root());
    if (root == nullptr) return error(SIRIUS_ENOMEM);
    fs->device_id = g_next_device++;
    g_root = root;
    LOG_INFO("vfs: %s mounted at /", fs->name());
    return 0;
}

int mount(Filesystem* fs, const char* path) {
    Lock lock;
    Dentry* target = nullptr;
    const int status = resolve(nullptr, path, Stop::AtEnd, target);
    if (status < 0) return status;
    if (!target->vnode->is_directory()) return error(SIRIUS_ENOTDIR);

    Dentry* root = new_dentry(nullptr, "/", fs->root());
    if (root == nullptr) return error(SIRIUS_ENOMEM);
    root->covers = target;
    target->mounted = root;   // `target` is already past any earlier mounts
    fs->device_id = g_next_device++;
    LOG_INFO("vfs: %s mounted at %s", fs->name(), path);
    return 0;
}

Dentry* root() { return g_root; }

void dentry_ref(Dentry* dentry) {
    Lock lock;
    dentry->refs++;
}

void dentry_unref(Dentry* dentry) {
    if (dentry == nullptr) return;
    Lock lock;
    if (dentry->refs == 0) panic("vfs: dentry '%s' over-released", dentry->name);
    dentry->refs--;
    free_if_dead(dentry);
}

// --- path operations ------------------------------------------------------------

int open(Dentry* cwd, const char* path, u32 flags, File*& out) {
    Lock lock;
    Dentry* dentry = nullptr;
    bool created = false;

    if (flags & SIRIUS_O_CREAT) {
        char leaf[path::kNameMax + 1];
        Dentry* parent = nullptr;
        int status = resolve(cwd, path, Stop::BeforeLast, parent, leaf);
        if (status < 0) return status;

        if (leaf[0] == '\0') {
            dentry = parent;                         // the root itself
        } else {
            status = child(parent, leaf, dentry);
            if (status == error(SIRIUS_ENOENT)) {
                if (path::Components::has_trailing_slash(path)) return error(SIRIUS_EISDIR);
                if (parent->vnode->fs()->read_only()) return error(SIRIUS_EROFS);
                Vnode* vnode = nullptr;
                status = parent->vnode->create(leaf, NodeType::Regular, vnode);
                if (status < 0) return status;
                dentry = new_dentry(parent, leaf, vnode);
                if (dentry == nullptr) { vnode->unref(); return error(SIRIUS_ENOMEM); }
                created = true;
            } else if (status < 0) {
                return status;
            } else if (flags & SIRIUS_O_EXCL) {
                return error(SIRIUS_EEXIST);
            }
        }
    } else {
        const int status = resolve(cwd, path, Stop::AtEnd, dentry);
        if (status < 0) return status;
    }

    Vnode* vnode = dentry->vnode;
    const bool directory = vnode->is_directory();
    if (directory && (wants_write(flags) || (flags & SIRIUS_O_TRUNC))) return error(SIRIUS_EISDIR);
    if (!directory && ((flags & SIRIUS_O_DIRECTORY) || path::Components::has_trailing_slash(path)))
        return error(SIRIUS_ENOTDIR);
    if (wants_write(flags) && vnode->fs()->read_only()) return error(SIRIUS_EROFS);

    if ((flags & SIRIUS_O_TRUNC) && !created && vnode->type() == NodeType::Regular) {
        const int status = vnode->truncate(0);
        if (status < 0) return status;
    }

    auto* file = static_cast<File*>(kzalloc(sizeof(File)));
    if (file == nullptr) return error(SIRIUS_ENOMEM);
    file->vnode  = vnode;
    file->dentry = dentry;
    file->flags  = flags;
    file->refs   = 1;
    vnode->ref();
    vnode->open_count++;
    dentry->refs++;

    out = file;
    return 0;
}

namespace {

class AnonymousFs final : public Filesystem {
public:
    const char* name() const override { return "anon"; }
    Vnode* root() override { return nullptr; }
    // Each object synchronises itself, and most of them block.
    bool serialized() const override { return false; }
};

} // namespace

// Made while the root is mounted, long before anything can open a pipe.
Filesystem* g_anonymous = nullptr;

Filesystem* anonymous_fs() { return g_anonymous; }

Filesystem* make_anonymous_fs() { return new AnonymousFs(); }

File* open_anonymous(Vnode* vnode, u32 flags) {
    auto* file = static_cast<File*>(kzalloc(sizeof(File)));
    if (file == nullptr) { vnode->unref(); return nullptr; }
    file->vnode = vnode;
    file->flags = flags;
    file->refs  = 1;
    return file;
}

int lookup(Dentry* cwd, const char* path, Vnode*& out) {
    Lock lock;
    Dentry* dentry = nullptr;
    const int status = resolve(cwd, path, Stop::AtEnd, dentry);
    if (status < 0) return status;
    dentry->vnode->ref();
    out = dentry->vnode;
    return 0;
}

int create_socket_node(Dentry* cwd, const char* path, Vnode*& out) {
    Lock lock;
    char leaf[path::kNameMax + 1];
    Dentry* parent = nullptr;
    int status = resolve(cwd, path, Stop::BeforeLast, parent, leaf);
    if (status < 0) return status;
    if (leaf[0] == '\0') return error(SIRIUS_EADDRINUSE);

    Dentry* existing = nullptr;
    status = child(parent, leaf, existing);
    if (status == 0) return error(SIRIUS_EADDRINUSE);
    if (status != error(SIRIUS_ENOENT)) return status;
    if (parent->vnode->fs()->read_only()) return error(SIRIUS_EROFS);

    Vnode* vnode = nullptr;
    status = parent->vnode->create(leaf, NodeType::Socket, vnode);
    if (status < 0) return status;
    Dentry* dentry = new_dentry(parent, leaf, vnode);
    if (dentry == nullptr) { vnode->unref(); return error(SIRIUS_ENOMEM); }
    vnode->ref();   // one for the dentry, one for the caller
    out = vnode;
    return 0;
}

int stat(Dentry* cwd, const char* path, sirius_stat& out) {
    Lock lock;
    Dentry* dentry = nullptr;
    const int status = resolve(cwd, path, Stop::AtEnd, dentry);
    if (status < 0) return status;
    fill_stat(dentry->vnode, out);
    return 0;
}

int mkdir(Dentry* cwd, const char* path) {
    Lock lock;
    char leaf[path::kNameMax + 1];
    Dentry* parent = nullptr;
    int status = resolve(cwd, path, Stop::BeforeLast, parent, leaf);
    if (status < 0) return status;
    if (leaf[0] == '\0') return error(SIRIUS_EEXIST);
    if (strcmp(leaf, "..") == 0) return error(SIRIUS_EEXIST);

    Dentry* existing = nullptr;
    status = child(parent, leaf, existing);
    if (status == 0) return error(SIRIUS_EEXIST);
    if (status != error(SIRIUS_ENOENT)) return status;
    if (!path::valid_new_name(leaf)) return error(SIRIUS_EINVAL);
    if (parent->vnode->fs()->read_only()) return error(SIRIUS_EROFS);

    Vnode* vnode = nullptr;
    status = parent->vnode->create(leaf, NodeType::Directory, vnode);
    if (status < 0) return status;
    if (new_dentry(parent, leaf, vnode) == nullptr) { vnode->unref(); return error(SIRIUS_ENOMEM); }
    return 0;
}

int unlink(Dentry* cwd, const char* path) {
    Lock lock;
    char leaf[path::kNameMax + 1];
    Dentry* parent = nullptr;
    int status = resolve(cwd, path, Stop::BeforeLast, parent, leaf);
    if (status < 0) return status;
    if (leaf[0] == '\0' || strcmp(leaf, "..") == 0) return error(SIRIUS_EBUSY);

    Dentry* victim = nullptr;
    status = child(parent, leaf, victim);
    if (status < 0) return status;
    // child() stepped over mounts; a mounted-on or mounted root is busy.
    if (victim->covers || victim->mounted) return error(SIRIUS_EBUSY);
    // FAT has no orphaned-but-open files: it would hand the clusters out
    // again while they are still being read. A filesystem that keeps its
    // data in the node itself lets the name go and the file live on.
    if (victim->vnode->open_count > 0 && !parent->vnode->fs()->keeps_unlinked_files()) return error(SIRIUS_EBUSY);
    if (parent->vnode->fs()->read_only()) return error(SIRIUS_EROFS);

    status = parent->vnode->unlink(leaf, victim->vnode);
    if (status < 0) return status;

    detach(victim);
    victim->unlinked = true;
    free_if_dead(victim);
    return 0;
}

int open_directory(Dentry* cwd, const char* path, Dentry*& out) {
    Lock lock;
    Dentry* dentry = nullptr;
    const int status = resolve(cwd, path, Stop::AtEnd, dentry);
    if (status < 0) return status;
    if (!dentry->vnode->is_directory()) return error(SIRIUS_ENOTDIR);
    dentry->refs++;
    out = dentry;
    return 0;
}

i64 path_of(Dentry* dentry, char* buffer, usize size) {
    Lock lock;
    if (size == 0) return error(SIRIUS_EINVAL);
    if (dentry == nullptr) dentry = g_root;
    if (dentry->unlinked) return error(SIRIUS_ENOENT);

    // Build the path backwards from the end of a scratch buffer.
    char scratch[path::kPathMax + 1];
    usize start = sizeof(scratch) - 1;
    scratch[start] = '\0';

    for (Dentry* d = dentry; d != g_root;) {
        while (d->covers) d = d->covers;
        if (d == g_root || d->parent == nullptr) break;
        const usize length = strlen(d->name);
        if (length + 1 > start) return error(SIRIUS_ENAMETOOLONG);
        start -= length;
        memcpy(&scratch[start], d->name, length);
        scratch[--start] = '/';
        d = d->parent;
    }
    if (start == sizeof(scratch) - 1) scratch[--start] = '/';

    const usize length = sizeof(scratch) - 1 - start;
    if (length + 1 > size) return error(SIRIUS_ENAMETOOLONG);
    memcpy(buffer, &scratch[start], length + 1);
    return static_cast<i64>(length);
}

// --- open-file operations -------------------------------------------------------

i64 read(File* file, void* buffer, u64 length) {
    if (!can_read(file->flags)) return error(SIRIUS_EBADF);
    Vnode* vnode = file->vnode;
    if (vnode->is_directory()) return error(SIRIUS_EISDIR);

    Lock lock(vnode->fs()->serialized());
    const i64 result = vnode->read(file->offset, buffer, length, file->flags);
    if (result > 0) file->offset += static_cast<u64>(result);
    return result;
}

i64 read_at(File* file, u64 offset, void* buffer, u64 length) {
    if (!can_read(file->flags)) return error(SIRIUS_EBADF);
    Vnode* vnode = file->vnode;
    if (vnode->is_directory()) return error(SIRIUS_EISDIR);
    Lock lock(vnode->fs()->serialized());
    return vnode->read(offset, buffer, length);
}

i64 write(File* file, const void* buffer, u64 length) {
    if (!wants_write(file->flags)) return error(SIRIUS_EBADF);
    Vnode* vnode = file->vnode;
    if (vnode->is_directory()) return error(SIRIUS_EISDIR);

    Lock lock(vnode->fs()->serialized());
    if (file->flags & SIRIUS_O_APPEND) file->offset = vnode->size();
    const i64 result = vnode->write(file->offset, buffer, length, file->flags);
    if (result > 0) file->offset += static_cast<u64>(result);
    return result;
}

i64 write_at(File* file, u64 offset, const void* buffer, u64 length) {
    if (!wants_write(file->flags)) return error(SIRIUS_EBADF);
    Vnode* vnode = file->vnode;
    if (vnode->is_directory()) return error(SIRIUS_EISDIR);
    Lock lock(vnode->fs()->serialized());
    return vnode->write(offset, buffer, length);
}

i64 seek(File* file, i64 offset, int whence) {
    Vnode* vnode = file->vnode;
    if (vnode->type() != NodeType::Regular && vnode->type() != NodeType::Directory && vnode->type() != NodeType::BlockDevice)
        return error(SIRIUS_ESPIPE);

    Lock lock(vnode->fs()->serialized());
    i64 base;
    switch (whence) {
    case SIRIUS_SEEK_SET: base = 0; break;
    case SIRIUS_SEEK_CUR: base = static_cast<i64>(file->offset); break;
    case SIRIUS_SEEK_END: base = static_cast<i64>(vnode->size()); break;
    default: return error(SIRIUS_EINVAL);
    }
    // A directory's offset is a filesystem cookie: only a rewind is meaningful.
    if (vnode->is_directory() && !(whence == SIRIUS_SEEK_SET && offset == 0)) return error(SIRIUS_EINVAL);
    if ((offset > 0 && base > INT64_MAX - offset) || base + offset < 0) return error(SIRIUS_EINVAL);
    file->offset = static_cast<u64>(base + offset);
    return base + offset;
}

int fstat(File* file, sirius_stat& out) {
    Lock lock(file->vnode->fs()->serialized());
    fill_stat(file->vnode, out);
    return 0;
}

int truncate(File* file, u64 size) {
    if (!wants_write(file->flags)) return error(SIRIUS_EBADF);
    if (file->vnode->type() != NodeType::Regular) return error(SIRIUS_EINVAL);
    Lock lock(file->vnode->fs()->serialized());
    return file->vnode->truncate(size);
}

namespace {

Spinlock g_flock_lock;

// Give up whatever lock `file` holds. Caller holds g_flock_lock.
void flock_release(File* file) {
    Vnode* vnode = file->vnode;
    if (file->flock == SIRIUS_LOCK_EX) vnode->flock_exclusive = nullptr;
    else if (file->flock == SIRIUS_LOCK_SH) vnode->flock_shared--;
    file->flock = 0;
}

} // namespace

int flock(File* file, u32 operation) {
    const bool nonblocking = operation & SIRIUS_LOCK_NB;
    const u32 wanted = operation & ~static_cast<u32>(SIRIUS_LOCK_NB);
    if (wanted != SIRIUS_LOCK_SH && wanted != SIRIUS_LOCK_EX && wanted != SIRIUS_LOCK_UN) return error(SIRIUS_EINVAL);

    Vnode* vnode = file->vnode;
    for (;;) {
        const u64 snapshot = events::snapshot();
        bool granted = false;
        {
            ScopedLock guard(g_flock_lock);
            if (wanted == SIRIUS_LOCK_UN || file->flock == wanted) {
                if (wanted == SIRIUS_LOCK_UN) flock_release(file);
                granted = true;
            } else {
                // Changing the kind of lock gives the old one up first: it
                // is not an atomic upgrade, on Linux either.
                const bool others_exclusive = vnode->flock_exclusive && vnode->flock_exclusive != file;
                const u32 others_shared = vnode->flock_shared - (file->flock == SIRIUS_LOCK_SH ? 1 : 0);
                if (!others_exclusive && (wanted == SIRIUS_LOCK_SH || others_shared == 0)) {
                    flock_release(file);
                    if (wanted == SIRIUS_LOCK_EX) vnode->flock_exclusive = file;
                    else vnode->flock_shared++;
                    file->flock = static_cast<u8>(wanted);
                    granted = true;
                }
            }
        }
        if (granted) {
            if (wanted == SIRIUS_LOCK_UN) events::notify();
            return 0;
        }
        if (nonblocking) return error(SIRIUS_EAGAIN);
        if (events::wait(snapshot) == -SIRIUS_EINTR) return error(SIRIUS_EINTR);
    }
}

u32 poll(File* file, u64& wake_at_ns) {
    return file->vnode->poll(wake_at_ns);
}

int ioctl(File* file, u64 request, u64 argument) {
    Lock lock(file->vnode->fs()->serialized());
    return file->vnode->ioctl(request, argument);
}

int readdir(File* file, sirius_dirent& out) {
    Vnode* vnode = file->vnode;
    if (!vnode->is_directory()) return error(SIRIUS_ENOTDIR);

    Lock lock;
    DirEntry entry;
    const int result = vnode->readdir(file->offset, entry);
    if (result <= 0) return result;

    out = {};
    out.d_ino = entry.inode;
    switch (entry.type) {
    case NodeType::Directory:   out.d_type = SIRIUS_DT_DIR; break;
    case NodeType::CharDevice:  out.d_type = SIRIUS_DT_CHR; break;
    case NodeType::BlockDevice: out.d_type = SIRIUS_DT_BLK; break;
    case NodeType::Regular:     out.d_type = SIRIUS_DT_REG; break;
    case NodeType::Fifo:        out.d_type = SIRIUS_DT_FIFO; break;
    case NodeType::Socket:      out.d_type = SIRIUS_DT_SOCK; break;
    }
    str_copy(out.d_name, sizeof(out.d_name), entry.name);
    return 1;
}

File* file_ref(File* file) {
    __atomic_fetch_add(&file->refs, 1, __ATOMIC_RELAXED);
    return file;
}

bool file_try_ref(File* file) {
    u32 refs = __atomic_load_n(&file->refs, __ATOMIC_RELAXED);
    while (refs != 0)
        if (__atomic_compare_exchange_n(&file->refs, &refs, refs + 1, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return true;
    return false;
}

void file_close(File* file) {
    if (file == nullptr) return;
    if (__atomic_sub_fetch(&file->refs, 1, __ATOMIC_ACQ_REL) != 0) return;

    // An epoll set watches open files, not descriptors, and must not be left
    // watching one that is gone.
    epoll::forget(file);

    if (file->flock) {
        {
            ScopedLock guard(g_flock_lock);
            flock_release(file);
        }
        events::notify();   // somebody may be waiting for it
    }

    if (file->dentry == nullptr) {
        // Not in the namespace, so nothing here needs the VFS lock -- and
        // the node's destructor may be closing files of its own (a socket
        // with descriptors still in flight), which would need it again.
        file->vnode->unref();
        kfree(file);
        return;
    }

    {
        Lock lock;
        file->vnode->open_count--;
        file->vnode->unref();
        Dentry* dentry = file->dentry;
        dentry->refs--;
        free_if_dead(dentry);
    }
    kfree(file);
}

int read_whole_file(Dentry* cwd, const char* path, void*& data, u64& size, u64 limit) {
    File* file = nullptr;
    int status = open(cwd, path, SIRIUS_O_RDONLY, file);
    if (status < 0) return status;

    if (file->vnode->type() != NodeType::Regular) { file_close(file); return error(SIRIUS_EACCES); }
    size = file->vnode->size();
    if (size > limit) { file_close(file); return error(SIRIUS_EFBIG); }

    data = kmalloc(size ? size : 1);
    if (data == nullptr) { file_close(file); return error(SIRIUS_ENOMEM); }

    u64 done = 0;
    while (done < size) {
        const i64 n = read(file, static_cast<u8*>(data) + done, size - done);
        if (n <= 0) { status = n < 0 ? static_cast<int>(n) : error(SIRIUS_EIO); break; }
        done += static_cast<u64>(n);
    }
    file_close(file);
    if (status < 0) { kfree(data); data = nullptr; }
    return status;
}

} // namespace vfs
