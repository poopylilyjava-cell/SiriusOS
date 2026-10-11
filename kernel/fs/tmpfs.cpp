#include <fs/tmpfs.hpp>
#include <mm/heap.hpp>
#include <mm/memory.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace tmpfs {
namespace {

using vfs::NodeType;
using vfs::Vnode;

constexpr u64 kPage = mm::kPageSize;
constexpr u64 kMaxFileSize = 64ULL * 1024 * 1024;  // keeps one file from eating RAM

class TmpFs;
class Node;

struct Link {
    char  name[path::kNameMax + 1];
    Node* node;
    Link* next;
};

class Node final : public Vnode {
public:
    Node(TmpFs* fs, NodeType type, u64 inode);
    ~Node() override;

    int lookup(const char* name, Vnode*& out) override;
    int create(const char* name, NodeType type, Vnode*& out) override;
    int unlink(const char* name, Vnode* victim) override;
    int readdir(u64& cookie, vfs::DirEntry& out) override;
    i64 read(u64 offset, void* buffer, u64 length) override;
    i64 write(u64 offset, const void* buffer, u64 length) override;
    int truncate(u64 size) override;
    u64 size() const override { return m_size; }

private:
    bool grow_page_table(u64 pages);

    // Directories: an unordered list of links. Files: a table of page
    // pointers, null for holes that have never been written.
    Link* m_links = nullptr;
    u8**  m_pages = nullptr;
    u64   m_page_slots = 0;
    u64   m_size = 0;
};

class TmpFs final : public vfs::Filesystem {
public:
    TmpFs() { m_root = new Node(this, NodeType::Directory, next_inode()); }
    ~TmpFs() override { m_root->unref(); }

    const char* name() const override { return "tmpfs"; }
    // A node lives as long as anything refers to it, name or no name.
    bool keeps_unlinked_files() const override { return true; }
    Vnode* root() override { m_root->ref(); return m_root; }
    u64 next_inode() { return m_next_inode++; }

private:
    Node* m_root;
    u64 m_next_inode = 1;
};

Node::Node(TmpFs* fs, NodeType type, u64 inode) : Vnode(fs, type, inode) {}

Node::~Node() {
    // A directory's entries each hold a reference to their node.
    while (Link* link = m_links) {
        m_links = link->next;
        link->node->unref();
        kfree(link);
    }
    for (u64 i = 0; i < m_page_slots; i++) kfree(m_pages[i]);
    kfree(m_pages);
}

int Node::lookup(const char* name, Vnode*& out) {
    if (!is_directory()) return -SIRIUS_ENOTDIR;
    for (Link* link = m_links; link; link = link->next) {
        if (strcmp(link->name, name) == 0) {
            link->node->ref();
            out = link->node;
            return 0;
        }
    }
    return -SIRIUS_ENOENT;
}

int Node::create(const char* name, NodeType type, Vnode*& out) {
    if (!is_directory()) return -SIRIUS_ENOTDIR;
    if (type != NodeType::Regular && type != NodeType::Directory && type != NodeType::Socket) return -SIRIUS_EINVAL;

    auto* link = static_cast<Link*>(kzalloc(sizeof(Link)));
    if (link == nullptr) return -SIRIUS_ENOMEM;
    auto* node = new Node(static_cast<TmpFs*>(fs()), type, static_cast<TmpFs*>(fs())->next_inode());

    str_copy(link->name, sizeof(link->name), name);
    link->node = node;           // the link owns the first reference
    link->next = m_links;
    m_links = link;

    node->ref();
    out = node;
    return 0;
}

int Node::unlink(const char* name, Vnode* victim) {
    if (!is_directory()) return -SIRIUS_ENOTDIR;
    auto* node = static_cast<Node*>(victim);
    if (node->is_directory() && node->m_links != nullptr) return -SIRIUS_ENOTEMPTY;

    for (Link** link = &m_links; *link; link = &(*link)->next) {
        if (strcmp((*link)->name, name) == 0) {
            Link* dead = *link;
            *link = dead->next;
            dead->node->unref();
            kfree(dead);
            return 0;
        }
    }
    return -SIRIUS_ENOENT;
}

int Node::readdir(u64& cookie, vfs::DirEntry& out) {
    if (!is_directory()) return -SIRIUS_ENOTDIR;
    // The cookie counts entries in creation order. New links go on the head,
    // so position `cookie` is `count - 1 - cookie` links in.
    u64 count = 0;
    for (Link* l = m_links; l; l = l->next) count++;
    if (cookie >= count) return 0;

    Link* link = m_links;
    for (u64 i = 0; i < count - 1 - cookie; i++) link = link->next;

    str_copy(out.name, sizeof(out.name), link->name);
    out.type = link->node->type();
    out.inode = link->node->inode();
    cookie++;
    return 1;
}

bool Node::grow_page_table(u64 pages) {
    if (pages <= m_page_slots) return true;
    u64 slots = m_page_slots ? m_page_slots : 4;
    while (slots < pages) slots *= 2;
    auto** table = static_cast<u8**>(kzalloc(slots * sizeof(u8*)));
    if (table == nullptr) return false;
    if (m_pages) memcpy(table, m_pages, m_page_slots * sizeof(u8*));
    kfree(m_pages);
    m_pages = table;
    m_page_slots = slots;
    return true;
}

i64 Node::read(u64 offset, void* buffer, u64 length) {
    if (offset >= m_size) return 0;
    if (length > m_size - offset) length = m_size - offset;

    auto* out = static_cast<u8*>(buffer);
    for (u64 done = 0; done < length;) {
        const u64 position = offset + done;
        const u64 in_page = position % kPage;
        const u64 n = (kPage - in_page) < (length - done) ? (kPage - in_page) : (length - done);
        const u8* page = m_pages[position / kPage];
        if (page) memcpy(out + done, page + in_page, n);
        else      memset(out + done, 0, n);
        done += n;
    }
    return static_cast<i64>(length);
}

i64 Node::write(u64 offset, const void* buffer, u64 length) {
    if (length == 0) return 0;
    if (offset > kMaxFileSize || length > kMaxFileSize - offset) return -SIRIUS_EFBIG;
    if (!grow_page_table((offset + length + kPage - 1) / kPage)) return -SIRIUS_ENOSPC;

    const auto* in = static_cast<const u8*>(buffer);
    u64 done = 0;
    while (done < length) {
        const u64 position = offset + done;
        const u64 in_page = position % kPage;
        const u64 n = (kPage - in_page) < (length - done) ? (kPage - in_page) : (length - done);
        u8*& page = m_pages[position / kPage];
        if (page == nullptr) {
            page = static_cast<u8*>(kzalloc(kPage));
            if (page == nullptr) {
                // Keep what was written; report the short write.
                if (done == 0) return -SIRIUS_ENOSPC;
                break;
            }
        }
        memcpy(page + in_page, in + done, n);
        done += n;
        if (position + n > m_size) m_size = position + n;
    }
    return static_cast<i64>(done);
}

int Node::truncate(u64 size) {
    if (size > kMaxFileSize) return -SIRIUS_EFBIG;
    if (size < m_size) {
        // Free whole pages past the new end, and zero the tail of the last
        // one so that growing the file again reads zeros.
        const u64 keep = (size + kPage - 1) / kPage;
        for (u64 i = keep; i < m_page_slots; i++) { kfree(m_pages[i]); m_pages[i] = nullptr; }
        if (size % kPage && m_pages[size / kPage])
            memset(m_pages[size / kPage] + size % kPage, 0, kPage - size % kPage);
    } else if (!grow_page_table((size + kPage - 1) / kPage)) {
        return -SIRIUS_ENOSPC;
    }
    m_size = size;
    return 0;
}

} // namespace

vfs::Filesystem* create() { return new TmpFs(); }

} // namespace tmpfs
