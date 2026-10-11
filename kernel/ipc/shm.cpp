// Anonymous shared memory (memfd_create): a file with no name, whose pages
// can be mapped MAP_SHARED by every process holding a descriptor for it.
// Passed over a socket, it is how a Wayland client's pixels reach the
// compositor without being copied.
#include <ipc/ipc.hpp>
#include <fs/vfs.hpp>
#include <sched/mutex.hpp>
#include <mm/heap.hpp>
#include <mm/memory.hpp>
#include <mm/pmm.hpp>
#include <mm/usercopy.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>
#include <sirius/fs.h>

namespace ipc {
namespace {

constexpr u64 kMaxSize = 256ULL * 1024 * 1024;

// Every page is allocated, and zero, from the moment the object is given its
// size: a mapping never has to fault one in, and two mappings of the same
// offset are the same physical page by construction.
//
// Guarded by a mutex of its own rather than ipc::g_lock: nothing here is
// reached from an interrupt, and reading and writing copy straight between
// the pages and user memory, which may fault.
class SharedMemory final : public vfs::Vnode {
public:
    SharedMemory() : Vnode(vfs::anonymous_fs(), vfs::NodeType::Regular, 0) {}

    ~SharedMemory() override {
        // Pages still mapped somewhere live on until they are unmapped.
        for (u64 i = 0; i < m_page_count; i++) pmm::decref(m_pages[i]);
        kfree(m_pages);
    }

    u64 size() const override { return __atomic_load_n(&m_size, __ATOMIC_RELAXED); }

    int truncate(u64 size) override {
        sched::ScopedMutex guard(m_mutex);
        return resize(size);
    }

    i64 read(u64 offset, void* buffer, u64 length) override {
        sched::ScopedMutex guard(m_mutex);
        if (offset >= m_size) return 0;
        if (length > m_size - offset) length = m_size - offset;
        copy(offset, buffer, length, false);
        return static_cast<i64>(length);
    }

    i64 write(u64 offset, const void* buffer, u64 length) override {
        sched::ScopedMutex guard(m_mutex);
        if (offset > kMaxSize || length > kMaxSize - offset) return -SIRIUS_EFBIG;
        if (offset + length > m_size) {
            if (const int status = resize(offset + length); status < 0) return status;
        }
        copy(offset, const_cast<void*>(buffer), length, true);
        return static_cast<i64>(length);
    }

    // With a reference taken for the mapping, so that a later shrink cannot
    // free the page out from under it.
    i64 shared_page(u64 offset) override {
        sched::ScopedMutex guard(m_mutex);
        if (offset % mm::kPageSize || offset / mm::kPageSize >= m_page_count) return -SIRIUS_EINVAL;
        const u64 physical = m_pages[offset / mm::kPageSize];
        pmm::incref(physical);
        return static_cast<i64>(physical);
    }

private:
    int resize(u64 size) {
        if (size > kMaxSize) return -SIRIUS_EFBIG;
        const u64 wanted = mm::pages_for(size);

        if (wanted > m_page_count) {
            auto* table = static_cast<u64*>(kzalloc(wanted * sizeof(u64)));
            if (table == nullptr) return -SIRIUS_ENOMEM;
            for (u64 i = m_page_count; i < wanted; i++) {
                table[i] = pmm::alloc_page_zeroed();
                if (table[i] == 0) {
                    for (u64 j = m_page_count; j < i; j++) pmm::free_page(table[j]);
                    kfree(table);
                    return -SIRIUS_ENOMEM;
                }
            }
            if (m_pages) memcpy(table, m_pages, m_page_count * sizeof(u64));
            kfree(m_pages);
            m_pages = table;
        } else {
            // Mappings of the pages cut off keep them alive, and keep seeing
            // them; the object just no longer does.
            for (u64 i = wanted; i < m_page_count; i++) pmm::decref(m_pages[i]);
            // The tail of the last page must read as zeros if the object
            // grows over it again.
            if (size < m_size && size % mm::kPageSize)
                memset(mm::phys_to_virt<u8>(m_pages[wanted - 1]) + size % mm::kPageSize, 0, mm::kPageSize - size % mm::kPageSize);
        }
        m_page_count = wanted;
        __atomic_store_n(&m_size, size, __ATOMIC_RELEASE);
        return 0;
    }

    // [offset, offset + length) lies inside the object.
    void copy(u64 offset, void* buffer, u64 length, bool writing) {
        auto* user = static_cast<u8*>(buffer);
        for (u64 done = 0; done < length;) {
            const u64 position = offset + done;
            const u64 in_page = position % mm::kPageSize;
            const u64 n = mm::kPageSize - in_page < length - done ? mm::kPageSize - in_page : length - done;
            u8* page = mm::phys_to_virt<u8>(m_pages[position / mm::kPageSize]) + in_page;
            if (writing) memcpy(page, user + done, n);
            else memcpy(user + done, page, n);
            done += n;
        }
    }

    sched::Mutex m_mutex;
    u64* m_pages = nullptr;   // physical addresses; this object holds one reference to each
    u64 m_page_count = 0;
    u64 m_size = 0;
};

} // namespace

vfs::Vnode* create_shared_memory(u64 size) {
    auto* object = new SharedMemory();
    if (object->truncate(size) < 0) { object->unref(); return nullptr; }
    return object;
}

i64 sys_memfd_create(u64 name_address, u64 flags) {
    // The name only labels the object for debugging on Linux; it is checked
    // and forgotten. MFD_CLOEXEC (1) is the one flag honoured.
    char name[256];
    if (const i64 status = usercopy::string_from(name, name_address, sizeof(name)); status < 0) return status;
    if (flags & ~3ULL) return -SIRIUS_EINVAL;
    return install(new SharedMemory(), SIRIUS_O_RDWR, (flags & 1) ? SIRIUS_O_CLOEXEC : 0);
}

} // namespace ipc
