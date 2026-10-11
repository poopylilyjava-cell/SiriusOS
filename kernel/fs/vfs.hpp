#pragma once
#include <klib/types.hpp>
#include <fs/path.hpp>
#include <sirius/fs.h>

// The virtual file system: one namespace of dentries over any number of
// mounted filesystems.
//
// Locking (M3): one VFS mutex serialises every namespace operation and every
// data operation on a filesystem that asks for it (Filesystem::serialized).
// Coarse, but it makes the dentry cache, mounts and the FAT driver's on-disk
// structures trivially consistent. Device filesystems opt out, so a slow
// device never holds up the disk.
namespace vfs {

enum class NodeType : u8 { Regular, Directory, CharDevice, BlockDevice, Fifo, Socket };

struct DirEntry {
    char     name[path::kNameMax + 1];
    NodeType type;
    u64      inode;
};

class Filesystem;

// A file, directory or device, as a filesystem presents it. Reference
// counted: a dentry holds one reference, every open File another. Methods are
// called with the VFS lock held if the filesystem is serialized.
class Vnode {
public:
    Vnode(Filesystem* fs, NodeType type, u64 inode) : m_fs(fs), m_type(type), m_inode(inode) {}
    virtual ~Vnode() = default;

    Filesystem* fs() const    { return m_fs; }
    NodeType    type() const  { return m_type; }
    u64         inode() const { return m_inode; }
    bool is_directory() const { return m_type == NodeType::Directory; }

    // --- directories. `out` receives a new reference. ---
    virtual int lookup(const char* name, Vnode*& out);
    virtual int create(const char* name, NodeType type, Vnode*& out);
    // `victim` is the vnode `name` refers to. A directory must be empty.
    virtual int unlink(const char* name, Vnode* victim);
    // Return the entry at `cookie` and advance it: 1, 0 at the end, or -errno.
    virtual int readdir(u64& cookie, DirEntry& out);

    // --- files and devices ---
    virtual i64 read(u64 offset, void* buffer, u64 length);
    // The same, for nodes whose reads depend on how the file was opened
    // (a device honouring SIRIUS_O_NONBLOCK). The default ignores the flags.
    virtual i64 read(u64 offset, void* buffer, u64 length, u32 open_flags);
    virtual i64 write(u64 offset, const void* buffer, u64 length);
    virtual i64 write(u64 offset, const void* buffer, u64 length, u32 open_flags);
    // What could be done now without blocking: SIRIUS_POLLIN, POLLOUT,
    // POLLHUP, POLLERR. A node whose answer will change at a known time
    // without any event announcing it (a timer) lowers `wake_at_ns` to that
    // time. Plain files are always ready for both.
    virtual u32 poll(u64& wake_at_ns);
    // For mmap(MAP_SHARED) of memory that is the node's own contents: the
    // physical page holding `offset` (page aligned), which the node keeps
    // alive for as long as it is part of the object; the mapping takes a
    // reference of its own. -ENODEV for nodes that cannot be shared.
    virtual i64 shared_page(u64 offset);
    // --- devices ---
    // A device-defined request; `argument` is usually a user pointer, which
    // the device must check (mm/usercopy.hpp). -ENOTTY: not understood.
    virtual int ioctl(u64 request, u64 argument);
    // For mmap(MAP_SHARED): the physical address of the page at `offset`
    // (page aligned) in the device's memory, or -errno if there is none.
    // `write_combining` asks for that caching mode, as framebuffers want.
    virtual i64 physical_page(u64 offset, bool& write_combining);
    virtual int truncate(u64 size);
    virtual u64 size() const { return 0; }

    void ref()   { __atomic_fetch_add(&m_refs, 1, __ATOMIC_RELAXED); }
    void unref();

    u32 open_count = 0;  // Files open on this vnode; an open file cannot be unlinked

    // flock() state, guarded by a lock in vfs.cpp.
    struct File* flock_exclusive = nullptr;
    u32 flock_shared = 0;

private:
    Filesystem* m_fs;
    NodeType m_type;
    u64 m_inode;
    u32 m_refs = 1;
};

class Filesystem {
public:
    virtual ~Filesystem() = default;
    virtual const char* name() const = 0;
    virtual Vnode* root() = 0;          // a new reference
    // Data operations run under the VFS lock unless this is false.
    virtual bool serialized() const { return true; }
    virtual bool read_only() const { return false; }
    // May a file be unlinked while it is open, living on until the last
    // close? Needs somewhere for a nameless file's data to stay.
    virtual bool keeps_unlinked_files() const { return false; }
    u32 device_id = 0;
};

// A name in the namespace. Lives in the dentry cache; leaves it only when
// unlinked, and is freed once nothing references it any more.
struct Dentry;

// An open file description: shared between processes by fork, so the offset
// is shared too, as POSIX requires.
struct File {
    Vnode* vnode;
    Dentry* dentry;   // null for objects with no name: pipes, sockets, memfds
    u64 offset;
    u32 flags;     // SIRIUS_O_*
    u32 refs;
    u8  flock;     // the flock() this file holds on its vnode: 0, SIRIUS_LOCK_SH or SIRIUS_LOCK_EX
};

// Mount the first filesystem at "/". Everything else mounts below it.
int mount_root(Filesystem* fs);
// Mount `fs` on the existing directory `path`.
int mount(Filesystem* fs, const char* path);

Dentry* root();
void dentry_ref(Dentry* dentry);
void dentry_unref(Dentry* dentry);

// Path operations. `cwd` anchors relative paths (null means "/").
int open(Dentry* cwd, const char* path, u32 flags, File*& out);
// An open file on a node that is in no directory. Takes over the caller's
// reference to `vnode`. Null if out of memory (the reference is dropped).
File* open_anonymous(Vnode* vnode, u32 flags);
// The filesystem such nodes belong to.
Filesystem* anonymous_fs();
// A new reference to the node at `path`.
int lookup(Dentry* cwd, const char* path, Vnode*& out);
// Create a socket's name in the filesystem: a node of type Socket that
// exists only to be found again by lookup(). -EADDRINUSE if the name is taken.
int create_socket_node(Dentry* cwd, const char* path, Vnode*& out);
int stat(Dentry* cwd, const char* path, sirius_stat& out);
int mkdir(Dentry* cwd, const char* path);
int unlink(Dentry* cwd, const char* path);
// A new reference to the directory at `path`, for chdir.
int open_directory(Dentry* cwd, const char* path, Dentry*& out);
// The absolute path of `dentry`; returns its length or -errno.
i64 path_of(Dentry* dentry, char* buffer, usize size);

// Open-file operations. Buffers may be user memory that the caller has
// validated: faults on it are resolved by the page fault handler.
i64 read(File* file, void* buffer, u64 length);
// Read at an explicit offset, leaving the file's own offset alone (pread).
i64 read_at(File* file, u64 offset, void* buffer, u64 length);
i64 write(File* file, const void* buffer, u64 length);
// Write at an explicit offset, leaving the file's own offset alone (pwrite).
i64 write_at(File* file, u64 offset, const void* buffer, u64 length);
i64 seek(File* file, i64 offset, int whence);
int fstat(File* file, sirius_stat& out);
int truncate(File* file, u64 size);
u32 poll(File* file, u64& wake_at_ns);
// SIRIUS_LOCK_*. Waits for a conflicting lock to go unless LOCK_NB is set;
// -EINTR if a signal arrives first.
int flock(File* file, u32 operation);
int ioctl(File* file, u64 request, u64 argument);
int readdir(File* file, sirius_dirent& out);

File* file_ref(File* file);
// Take a reference unless the file is already being closed (its count has
// reached zero): for code that finds a File through something other than a
// descriptor. False if it is on its way out.
bool  file_try_ref(File* file);
void  file_close(File* file);

// Read a whole regular file into a new kernel buffer (kfree it). For exec.
int read_whole_file(Dentry* cwd, const char* path, void*& data, u64& size, u64 limit);

} // namespace vfs
