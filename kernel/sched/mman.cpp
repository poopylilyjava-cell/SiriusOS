// mmap and munmap for the calling process.
//
// Anonymous memory is reserved and paged in on first touch; a private file
// mapping is read in eagerly, page by page, so later changes to the file do
// not show through and changes to the mapping never reach the file.
//
// A shared mapping is of memory that exists apart from the process: a
// device's own (the framebuffer, Vnode::physical_page) or a shared memory
// object's (memfd_create, Vnode::shared_page). Either is described page by
// page by its node. Files on a disk cannot be mapped shared: there is no page
// cache to share.
#include <sched/process.hpp>
#include <fs/vfs.hpp>
#include <ipc/ipc.hpp>
#include <mm/paging_math.hpp>
#include <mm/pmm.hpp>
#include <klib/string.hpp>
#include <sirius/syscall.h>

namespace sched {
namespace {

constexpr u64 kUserMin = 0x10000;                   // keep low memory, and null, unmapped
constexpr u64 kUserEnd = 0x0000800000000000ULL;

u64 page_flags_for(u64 protection) {
    u64 flags = vmm::flags::kPresent | vmm::flags::kUser;
    if (protection & SIRIUS_PROT_WRITE) flags |= vmm::flags::kWrite;
    if (!(protection & SIRIUS_PROT_EXEC)) flags |= vmm::flags::kNoExecute;
    return flags;
}

void release_range(vmm::AddressSpace& space, u64 address, u64 length) {
    for (u64 page = address; page < address + length; page += mm::kPageSize) space.release(page);
}

} // namespace

i64 mmap(u64 address, u64 length, u64 protection, u64 flags, i64 fd, u64 offset) {
    Process* process = current_process();
    const bool anonymous = (flags & SIRIUS_MAP_ANONYMOUS) != 0;

    if (length == 0) return -SIRIUS_EINVAL;
    const u64 sharing = flags & (SIRIUS_MAP_SHARED | SIRIUS_MAP_PRIVATE);
    if (sharing != SIRIUS_MAP_PRIVATE && sharing != SIRIUS_MAP_SHARED) return -SIRIUS_EINVAL;
    const bool shared = sharing == SIRIUS_MAP_SHARED;
    if (offset % mm::kPageSize) return -SIRIUS_EINVAL;
    if (length > kUserEnd) return -SIRIUS_ENOMEM;
    length = mm::align_up(length, mm::kPageSize);

    vfs::File* file = nullptr;
    bool device = false;
    // Shared and anonymous: a shared memory object nobody else can name. The
    // pages outlive it, kept by the mapping and by those fork makes of it.
    vfs::Vnode* nameless = nullptr;
    if (shared && anonymous) {
        if (protection == SIRIUS_PROT_NONE) return -SIRIUS_EINVAL;
        nameless = ipc::create_shared_memory(mm::align_up(length, mm::kPageSize));
        if (nameless == nullptr) return -SIRIUS_ENOMEM;
        offset = 0;
    }
    struct ReleaseNameless {
        vfs::Vnode*& node;
        ~ReleaseNameless() { if (node) node->unref(); }
    } release_nameless{nameless};
    if (!anonymous) {
        if (fd < 0 || (file = file_of(static_cast<u64>(fd))) == nullptr) return -SIRIUS_EBADF;
        if ((file->flags & SIRIUS_O_ACCMODE) == SIRIUS_O_WRONLY) return -SIRIUS_EACCES;
        if (shared) {
            // Every page must exist before anything is mapped (or, for a
            // fixed mapping, unmapped): a device with no memory to share
            // answers ENODEV for the first of them.
            if (protection == SIRIUS_PROT_NONE) return -SIRIUS_EINVAL;
            if ((protection & SIRIUS_PROT_WRITE) && (file->flags & SIRIUS_O_ACCMODE) != SIRIUS_O_RDWR) return -SIRIUS_EACCES;
            bool write_combining;
            const i64 first = file->vnode->physical_page(offset, write_combining);
            device = first != -SIRIUS_ENODEV;
            if (device) {
                for (u64 done = 0; done < length; done += mm::kPageSize) {
                    const i64 physical = file->vnode->physical_page(offset + done, write_combining);
                    if (physical < 0) return physical;
                }
            } else if (offset > file->vnode->size() || length > mm::align_up(file->vnode->size(), mm::kPageSize) - offset) {
                // A node that shares nothing answers ENODEV below; one that
                // does must cover the whole range.
                const i64 probe = file->vnode->shared_page(0);
                if (probe == -SIRIUS_ENODEV) return probe;
                if (probe >= 0) pmm::decref(static_cast<u64>(probe));
                return -SIRIUS_EINVAL;
            }
        } else if (file->vnode->type() != vfs::NodeType::Regular) {
            return -SIRIUS_EACCES;
        }
    }

    u64 base;
    if (flags & SIRIUS_MAP_FIXED) {
        if (address % mm::kPageSize || address < kUserMin || address >= kUserEnd ||
            length > kUserEnd - address) return -SIRIUS_EINVAL;
        base = address;
        release_range(process->space, base, length);   // a fixed mapping replaces what was there
    } else {
        // A bump allocator: addresses are never reused, which is plenty for
        // M3 and means a stale pointer into an unmapped region always faults.
        // (Atomically: another thread may be mapping too.)
        if (length > kUserEnd - kUserMin) return -SIRIUS_ENOMEM;
        base = __atomic_fetch_add(&process->mmap_next, length + mm::kPageSize, __ATOMIC_RELAXED);   // a guard gap between mappings
        if (base >= kUserEnd || length > kUserEnd - base - mm::kPageSize) return -SIRIUS_ENOMEM;
    }

    if (protection == SIRIUS_PROT_NONE) return static_cast<i64>(base);   // reserved, inaccessible
    const u64 page_flags = page_flags_for(protection);

    for (u64 done = 0; done < length; done += mm::kPageSize) {
        const u64 page = base + done;
        if (shared && !device) {
            // The node hands over a reference to the frame with its address.
            const i64 physical = (nameless ? nameless : file->vnode)->shared_page(offset + done);
            if (physical < 0 || !process->space.map_shared(page, static_cast<u64>(physical), page_flags | vmm::flags::kNoExecute)) {
                if (physical >= 0) pmm::decref(static_cast<u64>(physical));
                release_range(process->space, base, done);
                return physical < 0 ? physical : -SIRIUS_ENOMEM;
            }
            continue;
        }
        if (shared) {
            bool write_combining = false;
            const i64 physical = file->vnode->physical_page(offset + done, write_combining);
            // Device memory is data, never code.
            if (physical < 0 || !process->space.map_device(page, static_cast<u64>(physical), page_flags | vmm::flags::kNoExecute,
                                    write_combining ? vmm::MemoryType::WriteCombining : vmm::MemoryType::Normal)) {
                release_range(process->space, base, done);
                return physical < 0 ? physical : -SIRIUS_ENOMEM;
            }
            continue;
        }
        if (anonymous) {
            if (!process->space.map_demand(page, page_flags)) { release_range(process->space, base, done); return -SIRIUS_ENOMEM; }
            continue;
        }

        const u64 physical = pmm::alloc_page_zeroed();
        if (physical == 0) { release_range(process->space, base, done); return -SIRIUS_ENOMEM; }
        // Past the end of the file the page simply stays zero.
        const i64 n = vfs::read_at(file, offset + done, mm::phys_to_virt<void>(physical), mm::kPageSize);
        if (n < 0 || !process->space.map(page, physical, page_flags)) {
            pmm::free_page(physical);
            release_range(process->space, base, done);
            return n < 0 ? n : -SIRIUS_ENOMEM;
        }
    }
    return static_cast<i64>(base);
}

i64 munmap(u64 address, u64 length) {
    if (address % mm::kPageSize || length == 0) return -SIRIUS_EINVAL;
    if (address < kUserMin || address >= kUserEnd || length > kUserEnd - address) return -SIRIUS_EINVAL;
    release_range(current_process()->space, address, mm::align_up(length, mm::kPageSize));
    return 0;
}

} // namespace sched
