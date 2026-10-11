// The FAT32 volume as a VFS filesystem.
//
// FAT has no inode numbers and its lookups ignore case, so "/motd" and
// "/MOTD" reach the same directory entry through two different dentries. The
// adapter keeps exactly one live vnode per entry -- keyed by where the entry
// sits on disk -- so both names share one fat::Node and one file size.
#include <fs/fat/fatfs.hpp>
#include <fs/fat/volume.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace fatfs {
namespace {

using vfs::NodeType;
using vfs::Vnode;

class FatFs;

class FatVnode final : public Vnode {
public:
    FatVnode(FatFs* fs, const fat::Node& node);
    ~FatVnode() override;

    int lookup(const char* name, Vnode*& out) override;
    int create(const char* name, NodeType type, Vnode*& out) override;
    int unlink(const char* name, Vnode* victim) override;
    int readdir(u64& cookie, vfs::DirEntry& out) override;
    i64 read(u64 offset, void* buffer, u64 length) override;
    i64 write(u64 offset, const void* buffer, u64 length) override;
    int truncate(u64 size) override;
    u64 size() const override { return m_node.size; }

    fat::Node& node() { return m_node; }
    FatVnode* next_live = nullptr;

private:
    FatFs* owner() { return reinterpret_cast<FatFs*>(fs()); }
    fat::Node m_node;
};

u64 inode_for(const fat::Node& node) {
    if (node.is_root) return 1;
    return (static_cast<u64>(node.parent_cluster) << 20) ^ node.entry_index ^ (1ULL << 63);
}

class FatFs final : public vfs::Filesystem {
public:
    explicit FatFs(block::Device* device) : m_device(device) {}

    int start() {
        const int status = m_volume.mount(m_device);
        if (status < 0) return status;
        m_root = new FatVnode(this, m_volume.root());
        const fat::Geometry g = m_volume.geometry();
        LOG_INFO("fat: %s: %u clusters of %u KiB, %u free", m_device->name(), g.cluster_count,
                 g.sectors_per_cluster / 2, g.free_clusters);
        return 0;
    }

    const char* name() const override { return "fat32"; }
    Vnode* root() override { m_root->ref(); return m_root; }

    fat::Volume& volume() { return m_volume; }

    // The vnode for this entry: the live one if there is one, else a new one.
    Vnode* vnode_for(const fat::Node& node) {
        for (FatVnode* v = m_live; v; v = v->next_live) {
            const fat::Node& n = v->node();
            if (n.parent_cluster == node.parent_cluster && n.entry_index == node.entry_index) {
                v->ref();
                return v;
            }
        }
        auto* vnode = new FatVnode(this, node);
        vnode->next_live = m_live;
        m_live = vnode;
        return vnode;
    }

    void forget(FatVnode* vnode) {
        for (FatVnode** link = &m_live; *link; link = &(*link)->next_live) {
            if (*link == vnode) { *link = vnode->next_live; return; }
        }
    }

    // Every change is pushed to the disk before the call returns: the cache
    // below is write-through, and this writes FSInfo and flushes the drive.
    int commit() { return m_volume.sync(); }

private:
    block::Device* m_device;
    fat::Volume m_volume;
    FatVnode* m_root = nullptr;
    FatVnode* m_live = nullptr;   // every vnode but the root
};

FatVnode::FatVnode(FatFs* fs, const fat::Node& node)
    : Vnode(fs, node.directory ? NodeType::Directory : NodeType::Regular, inode_for(node)), m_node(node) {}

FatVnode::~FatVnode() {
    if (!m_node.is_root) owner()->forget(this);
}

int FatVnode::lookup(const char* name, Vnode*& out) {
    fat::Node child;
    const int status = owner()->volume().lookup(m_node, name, child);
    if (status < 0) return status;
    out = owner()->vnode_for(child);
    return 0;
}

int FatVnode::create(const char* name, NodeType type, Vnode*& out) {
    if (type != NodeType::Regular && type != NodeType::Directory) return -SIRIUS_EINVAL;
    fat::Node child;
    int status = owner()->volume().create(m_node, name, type == NodeType::Directory, child);
    if (status < 0) return status;
    status = owner()->commit();
    out = owner()->vnode_for(child);
    return status < 0 ? status : 0;
}

int FatVnode::unlink(const char* name, Vnode* victim) {
    fat::Node& node = static_cast<FatVnode*>(victim)->node();
    const int status = owner()->volume().unlink(m_node, name, node);
    if (status < 0) return status;
    // The slot may be reused by the next create. The old vnode can outlive
    // its name (a cwd still holding it), so it must stop matching the slot.
    node.parent_cluster = 0xffffffff;
    node.entry_index = 0xffffffff;
    return owner()->commit();
}

int FatVnode::readdir(u64& cookie, vfs::DirEntry& out) {
    fat::Entry entry;
    const int status = owner()->volume().readdir(m_node, cookie, entry);
    if (status <= 0) return status;
    str_copy(out.name, sizeof(out.name), entry.name);
    out.type = entry.node.directory ? NodeType::Directory : NodeType::Regular;
    out.inode = inode_for(entry.node);
    return 1;
}

i64 FatVnode::read(u64 offset, void* buffer, u64 length) {
    return owner()->volume().read(m_node, offset, buffer, length);
}

i64 FatVnode::write(u64 offset, const void* buffer, u64 length) {
    const i64 written = owner()->volume().write(m_node, offset, buffer, length);
    if (written <= 0) return written;
    const int status = owner()->commit();
    return status < 0 ? status : written;
}

int FatVnode::truncate(u64 size) {
    const int status = owner()->volume().truncate(m_node, size);
    if (status < 0) return status;
    return owner()->commit();
}

} // namespace

vfs::Filesystem* mount(block::Device* device, int& error) {
    auto* fs = new FatFs(device);
    error = fs->start();
    if (error < 0) { delete fs; return nullptr; }
    return fs;
}

} // namespace fatfs
