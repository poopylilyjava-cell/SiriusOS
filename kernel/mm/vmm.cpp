#include <mm/vmm.hpp>
#include <mm/pmm.hpp>
#include <mm/paging_math.hpp>
#include <boot/requests.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <klib/spinlock.hpp>
#include <arch/x86_64/percpu.hpp>
#include <limine.h>

extern "C" {
extern char __text_start[],   __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[],   __kernel_end[];
}

namespace vmm {
namespace {

AddressSpace g_kernel_space;
// Serialises changes to the shared kernel half after boot, and the bump
// pointers handing out its stack and MMIO ranges.
Spinlock g_kernel_map_lock;
Stats g_stats{};

constexpr u64 kEntriesPerTable = 512;
constexpr u64 kLargePageSize   = mm::kLargePageSize;

u64* table_at(u64 physical) { return mm::phys_to_virt<u64>(physical); }

u64 index_for(u64 virtual_address, int level) { return mm::page_table_index(virtual_address, level); }

// Walk one level down, optionally allocating the next table. Intermediate
// entries are permissive (writable, and user-accessible when the leaf will be):
// the CPU ands the permissions along the whole walk, so a restrictive
// intermediate would silently override the leaf.
u64* descend(u64* table, u64 virtual_address, int level, bool create_missing, u64 leaf_flags) {
    u64& entry = table[index_for(virtual_address, level)];

    if (!(entry & flags::kPresent)) {
        if (!create_missing) return nullptr;

        const u64 page = pmm::alloc_page_zeroed();
        if (page == 0) return nullptr;
        __atomic_fetch_add(&g_stats.page_tables_allocated, 1, __ATOMIC_RELAXED);

        entry = page | flags::kPresent | flags::kWrite;
        if (leaf_flags & flags::kUser) entry |= flags::kUser;
    } else if ((leaf_flags & flags::kUser) && !(entry & flags::kUser)) {
        // An existing kernel-only intermediate has to be widened before a user
        // leaf beneath it can ever be reached.
        entry |= flags::kUser;
    }

    if (entry & flags::kHuge) return nullptr;  // a 2 MiB mapping is already a leaf
    return table_at(entry & flags::kAddressMask);
}

u64 memory_type_bits(MemoryType type, bool large) {
    if (type != MemoryType::WriteCombining) return 0;
    return large ? flags::kWriteCombiningLarge : flags::kWriteCombiningSmall;
}

} // namespace

// A user address space is changed only on the CPU its process's threads run
// on. With interrupts off there, an operation cannot be preempted by another
// of those threads, nor interleaved with a page fault of theirs: it is atomic.
struct Uninterrupted {
    u64 flags = irq::save();
    ~Uninterrupted() { irq::restore(flags); }
};

void invalidate(u64 virtual_address) {
    __asm__ volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

u64* AddressSpace::entry_for(u64 virtual_address, bool create_missing, u64 leaf_flags) {
    if (m_root == 0) return nullptr;

    u64* table = table_at(m_root);
    for (int level = 4; level > 1; level--) {
        table = descend(table, virtual_address, level, create_missing, leaf_flags);
        if (table == nullptr) return nullptr;
    }
    return &table[index_for(virtual_address, 1)];
}

bool AddressSpace::map(u64 virtual_address, u64 physical, u64 page_flags) {
    if (m_root == 0) return false;
    const Uninterrupted atomic;

    u64* table = table_at(m_root);
    for (int level = 4; level > 1; level--) {
        table = descend(table, virtual_address, level, true, page_flags);
        if (table == nullptr) return false;
    }

    u64& entry = table[index_for(virtual_address, 1)];
    entry = (physical & flags::kAddressMask) | page_flags;
    invalidate(virtual_address);
    return true;
}

bool AddressSpace::map_range(u64 virtual_address, u64 physical, u64 size,
                             u64 page_flags, MemoryType type) {
    const u64 end = virtual_address + size;

    while (virtual_address < end) {
        // 2 MiB pages wherever alignment and remaining size allow: mapping the
        // whole direct map with 4 KiB entries would cost tens of MiB of tables.
        const bool can_use_large =
            (virtual_address % kLargePageSize == 0) &&
            (physical % kLargePageSize == 0) &&
            (end - virtual_address >= kLargePageSize);

        if (can_use_large) {
            u64* table = table_at(m_root);
            for (int level = 4; level > 2; level--) {
                table = descend(table, virtual_address, level, true, page_flags);
                if (table == nullptr) return false;
            }
            table[index_for(virtual_address, 2)] =
                (physical & flags::kAddressMask) | page_flags | flags::kHuge |
                memory_type_bits(type, true);
            invalidate(virtual_address);

            virtual_address += kLargePageSize;
            physical        += kLargePageSize;
        } else {
            if (!map(virtual_address, physical, page_flags | memory_type_bits(type, false)))
                return false;
            virtual_address += mm::kPageSize;
            physical        += mm::kPageSize;
        }
    }
    return true;
}

namespace {
bool table_is_empty(const u64* table) {
    for (u64 i = 0; i < kEntriesPerTable; i++)
        if (table[i] != 0) return false;
    return true;
}
} // namespace

bool AddressSpace::unmap(u64 virtual_address) {
    if (m_root == 0) return false;

    // Record the table at each level on the way down, so emptied tables can be
    // released on the way back up. Without this, mapping and unmapping churns
    // page tables that are never reclaimed.
    u64* tables[4];  // [3] = PML4, [2] = PDPT, [1] = PD, [0] = PT
    tables[3] = table_at(m_root);

    u64* table = tables[3];
    for (int level = 4; level > 1; level--) {
        const u64 entry = table[index_for(virtual_address, level)];
        if (!(entry & flags::kPresent)) return false;
        if (entry & flags::kHuge) return false;  // 2 MiB mappings are not unmapped piecemeal

        table = table_at(entry & flags::kAddressMask);
        tables[level - 2] = table;
    }

    u64& leaf = tables[0][index_for(virtual_address, 1)];
    if (!(leaf & (flags::kPresent | flags::kDemand))) return false;

    leaf = 0;
    invalidate(virtual_address);

    for (int level = 1; level <= 3; level++) {
        if (!table_is_empty(tables[level - 1])) break;

        // The kernel-half PML4 entries are pre-allocated once and copied by
        // value into every address space. Freeing one here would leave dangling
        // entries in all the others.
        if (level == 3 && index_for(virtual_address, 4) >= 256) break;

        u64& parent_entry = tables[level][index_for(virtual_address, level + 1)];
        pmm::free_page(parent_entry & flags::kAddressMask);
        parent_entry = 0;
        __atomic_fetch_add(&g_stats.page_tables_freed, 1, __ATOMIC_RELAXED);
    }

    return true;
}

u64 AddressSpace::translate(u64 virtual_address) const {
    if (m_root == 0) return 0;

    u64* table = table_at(m_root);
    for (int level = 4; level > 1; level--) {
        const u64 entry = table[index_for(virtual_address, level)];
        if (!(entry & flags::kPresent)) return 0;

        if (entry & flags::kHuge) {
            // Only the PD level can be huge in our mappings.
            const u64 mask = kLargePageSize - 1;
            return (entry & flags::kAddressMask & ~mask) | (virtual_address & mask);
        }
        table = table_at(entry & flags::kAddressMask);
    }

    const u64 entry = table[index_for(virtual_address, 1)];
    if (!(entry & flags::kPresent)) return 0;
    return (entry & flags::kAddressMask) | (virtual_address & (mm::kPageSize - 1));
}

bool AddressSpace::map_demand(u64 virtual_address, u64 page_flags) {
    const Uninterrupted atomic;
    u64* entry = entry_for(virtual_address, true, page_flags);
    if (entry == nullptr) return false;

    // Present is clear, so the hardware ignores everything else in the entry --
    // which lets us park the intended flags here until the fault arrives.
    *entry = (page_flags & ~flags::kPresent) | flags::kDemand;
    invalidate(virtual_address);
    return true;
}

bool AddressSpace::map_device(u64 virtual_address, u64 physical, u64 page_flags, MemoryType type) {
    return map(virtual_address, physical, page_flags | flags::kDevice | memory_type_bits(type, false));
}

bool AddressSpace::map_shared(u64 virtual_address, u64 physical, u64 page_flags) {
    return map(virtual_address, physical, page_flags | flags::kShared);
}

bool AddressSpace::share_copy_on_write(AddressSpace& target, u64 virtual_address) {
    u64* source = entry_for(virtual_address, false);
    if (source == nullptr || !(*source & flags::kPresent)) return false;

    const u64 physical = *source & flags::kAddressMask;

    // Reach the target's entry before touching the source, so a failure here
    // leaves the source mapping exactly as it was.
    u64* destination = target.entry_for(virtual_address, true, *source);
    if (destination == nullptr) return false;

    // Both sides lose write permission and gain the marker; the first writer
    // takes the fault and pays for the copy. A page that was never writable
    // gets no marker: the fault handler restores write access to anything
    // carrying it, which would turn shared read-only pages (program text)
    // writable. Such a page is simply shared, and a write to it stays fatal.
    if (*source & (flags::kWrite | flags::kCopyOnWrite)) {
        *source = (*source & ~flags::kWrite) | flags::kCopyOnWrite;
        invalidate(virtual_address);
    }
    *destination = *source;

    pmm::incref(physical);
    return true;
}

bool AddressSpace::fork_into(AddressSpace& child) {
    // As a whole: a thread that ran in the middle could write to a page
    // already shared with the child as it was.
    const Uninterrupted atomic;
    const u64* pml4 = table_at(m_root);
    for (u64 i = 0; i < 256; i++) {
        if (!(pml4[i] & flags::kPresent)) continue;
        const u64* pdpt = table_at(pml4[i] & flags::kAddressMask);
        for (u64 j = 0; j < kEntriesPerTable; j++) {
            if (!(pdpt[j] & flags::kPresent) || (pdpt[j] & flags::kHuge)) continue;
            const u64* pd = table_at(pdpt[j] & flags::kAddressMask);
            for (u64 k = 0; k < kEntriesPerTable; k++) {
                if (!(pd[k] & flags::kPresent) || (pd[k] & flags::kHuge)) continue;
                const u64* pt = table_at(pd[k] & flags::kAddressMask);
                for (u64 l = 0; l < kEntriesPerTable; l++) {
                    const u64 entry = pt[l];
                    if (!(entry & (flags::kPresent | flags::kDemand))) continue;
                    const u64 address = (i << 39) | (j << 30) | (k << 21) | (l << 12);

                    if (entry & flags::kDevice) {
                        // Both processes see the device itself.
                        u64* target = child.entry_for(address, true, entry);
                        if (target == nullptr) return false;
                        *target = entry;
                    } else if ((entry & flags::kPresent) && (entry & flags::kShared)) {
                        // Shared memory stays shared: the same frame, one
                        // more reference, and still writable in both.
                        u64* target = child.entry_for(address, true, entry);
                        if (target == nullptr) return false;
                        *target = entry;
                        pmm::incref(entry & flags::kAddressMask);
                    } else if (entry & flags::kPresent) {
                        if (!share_copy_on_write(child, address)) return false;
                    } else {
                        u64* target = child.entry_for(address, true, entry);
                        if (target == nullptr) return false;
                        *target = entry;
                    }
                }
            }
        }
    }
    return true;
}

void AddressSpace::release(u64 virtual_address) {
    const Uninterrupted atomic;
    const u64* entry = entry_for(virtual_address, false);
    if (entry == nullptr || !(*entry & (flags::kPresent | flags::kDemand))) return;
    const u64 physical = (*entry & flags::kPresent) && !(*entry & flags::kDevice) ? (*entry & flags::kAddressMask) : 0;
    unmap(virtual_address);
    // Frames the allocator does not own (none are mapped into user space
    // today, but MMIO will be) have no count to drop.
    if (physical && pmm::refcount(physical) != 0) pmm::decref(physical);
}

bool AddressSpace::create() {
    m_root = pmm::alloc_page_zeroed();
    if (m_root == 0) return false;

    // Share the kernel half by copying the top 256 PML4 entries. Those point at
    // tables the kernel space pre-allocated, so later kernel mappings are
    // visible here too without any synchronisation.
    if (g_kernel_space.valid()) {
        const u64* kernel_table = table_at(g_kernel_space.root());
        u64* table = table_at(m_root);
        for (u64 i = 256; i < kEntriesPerTable; i++) table[i] = kernel_table[i];
    }
    return true;
}

void AddressSpace::destroy() {
    if (m_root == 0) return;

    // Only the lower half belongs to this space; the kernel half is shared.
    u64* pml4 = table_at(m_root);
    for (u64 i = 0; i < 256; i++) {
        if (!(pml4[i] & flags::kPresent)) continue;
        u64* pdpt = table_at(pml4[i] & flags::kAddressMask);

        for (u64 j = 0; j < kEntriesPerTable; j++) {
            if (!(pdpt[j] & flags::kPresent) || (pdpt[j] & flags::kHuge)) continue;
            u64* pd = table_at(pdpt[j] & flags::kAddressMask);

            for (u64 k = 0; k < kEntriesPerTable; k++) {
                // 2 MiB leaves are only ever made by map_range() over memory
                // the caller owns, never by the allocator, so they hold no
                // page reference to give back.
                if (!(pd[k] & flags::kPresent) || (pd[k] & flags::kHuge)) continue;
                u64* pt = table_at(pd[k] & flags::kAddressMask);

                for (u64 l = 0; l < kEntriesPerTable; l++) {
                    // A demand reservation that was never touched has no page.
                    if (!(pt[l] & flags::kPresent) || (pt[l] & flags::kDevice)) continue;
                    // Mapped memory the allocator does not own (MMIO, firmware
                    // regions) has no frame or a zero count: not ours to free.
                    const u64 physical = pt[l] & flags::kAddressMask;
                    if (pmm::refcount(physical) != 0) pmm::decref(physical);
                }
                pmm::free_page(pd[k] & flags::kAddressMask);
            }
            pmm::free_page(pdpt[j] & flags::kAddressMask);
        }
        pmm::free_page(pml4[i] & flags::kAddressMask);
    }

    pmm::free_page(m_root);
    m_root = 0;
}

void AddressSpace::activate() const {
    // A fault must be resolved against whatever is actually in CR3, so each
    // CPU records its own.
    percpu::current()->address_space = const_cast<AddressSpace*>(this);
    __asm__ volatile("mov %0, %%cr3" : : "r"(m_root) : "memory");
}

AddressSpace& kernel_space() { return g_kernel_space; }

namespace {
// Kernel stacks live in their own PML4 slot, well clear of the direct map and
// the kernel image. Each stack is preceded by one unmapped guard page; the
// next stack's guard doubles as this one's upper guard.
constexpr u64 kStackRegionBase = 0xffffc00000000000ULL;
u64 g_next_stack = kStackRegionBase;

// Device registers get the next PML4 slot. Uncached: PCD and PWT both set
// select PAT entry 3, which is UC in the power-on PAT that Limine keeps.
constexpr u64 kMmioRegionBase = 0xffffc08000000000ULL;
u64 g_next_mmio = kMmioRegionBase;
constexpr u64 kUncached = flags::kNoCache | flags::kWriteThrough;
} // namespace

u64 create_kernel_stack(u64 pages) {
    ScopedLock guard(g_kernel_map_lock);
    const u64 bottom = g_next_stack + mm::kPageSize;  // skip the guard page

    for (u64 i = 0; i < pages; i++) {
        const u64 page = pmm::alloc_page_zeroed();
        if (page == 0 ||
            !g_kernel_space.map(bottom + i * mm::kPageSize, page,
                                flags::kPresent | flags::kWrite | flags::kNoExecute)) {
            if (page != 0) pmm::free_page(page);
            for (u64 j = 0; j < i; j++) {
                const u64 address = bottom + j * mm::kPageSize;
                const u64 mapped = g_kernel_space.translate(address);
                g_kernel_space.unmap(address);
                pmm::free_page(mapped);
            }
            return 0;
        }
    }

    const u64 top = bottom + pages * mm::kPageSize;
    g_next_stack = top;
    LOG_DEBUG("vmm: kernel stack %#lx-%#lx, guard page at %#lx", bottom, top, bottom - mm::kPageSize);
    return top;
}

void destroy_kernel_stack(u64 stack_top, u64 pages) {
    ScopedLock guard(g_kernel_map_lock);
    const u64 bottom = stack_top - pages * mm::kPageSize;
    for (u64 i = 0; i < pages; i++) {
        const u64 address = bottom + i * mm::kPageSize;
        const u64 physical = g_kernel_space.translate(address);
        if (physical == 0) panic("vmm: destroying kernel stack %#lx: page %#lx not mapped", stack_top, address);
        g_kernel_space.unmap(address);
        pmm::free_page(physical);
    }
}

u64 map_mmio(u64 physical, u64 size) {
    ScopedLock guard(g_kernel_map_lock);
    const u64 first = mm::align_down(physical, mm::kPageSize);
    const u64 bytes = mm::align_up(physical + size, mm::kPageSize) - first;
    const u64 base  = g_next_mmio;

    for (u64 offset = 0; offset < bytes; offset += mm::kPageSize) {
        if (!g_kernel_space.map(base + offset, first + offset,
                                flags::kPresent | flags::kWrite | flags::kNoExecute | kUncached))
            return 0;
    }
    // A guard page between consecutive regions, so an overrun faults.
    g_next_mmio = base + bytes + mm::kPageSize;
    return base + (physical - first);
}

Stats stats() { return g_stats; }

void init(const limine_memmap_response* memmap) {
    if (memmap == nullptr) panic("vmm: no memory map");
    if (g_executable_address_request.response == nullptr)
        panic("vmm: bootloader did not report the executable address");

    const u64 physical_base = g_executable_address_request.response->physical_base;
    const u64 virtual_base  = g_executable_address_request.response->virtual_base;

    if (!g_kernel_space.create()) panic("vmm: cannot allocate the kernel PML4");

    // Pre-allocate every higher-half PML4 entry now. Address spaces created
    // later copy these 256 entries once; because the entries themselves never
    // change afterwards, a kernel mapping made at any later point is
    // automatically visible in every address space.
    u64* pml4 = table_at(g_kernel_space.root());
    for (u64 i = 256; i < kEntriesPerTable; i++) {
        const u64 page = pmm::alloc_page_zeroed();
        if (page == 0) panic("vmm: out of memory pre-allocating the kernel half");
        pml4[i] = page | flags::kPresent | flags::kWrite;
        __atomic_fetch_add(&g_stats.page_tables_allocated, 1, __ATOMIC_RELAXED);
    }

    // Rebuild the direct map. Reserved and bad regions are deliberately left
    // out: base revision 6 does not map them either, and mapping them would
    // hand the kernel pointers into firmware memory.
    const u64 hhdm = mm::hhdm_offset();
    u64 mapped_bytes = 0;
    for (u64 i = 0; i < memmap->entry_count; i++) {
        const auto* entry = memmap->entries[i];
        switch (entry->type) {
        case LIMINE_MEMMAP_RESERVED:
        case LIMINE_MEMMAP_BAD_MEMORY:
            continue;
        default:
            break;
        }

        const u64 base = mm::align_down(entry->base, mm::kPageSize);
        const u64 size = mm::align_up(entry->base + entry->length, mm::kPageSize) - base;
        const auto type = (entry->type == LIMINE_MEMMAP_FRAMEBUFFER)
                        ? MemoryType::WriteCombining : MemoryType::Normal;

        if (!g_kernel_space.map_range(hhdm + base, base, size,
                                      flags::kPresent | flags::kWrite | flags::kNoExecute, type))
            panic("vmm: failed to map physical %#lx+%#lx into the direct map", base, size);
        mapped_bytes += size;
    }

    // Map the kernel image itself with per-section permissions, rather than one
    // blanket writable-executable mapping.
    struct Section { const char* name; u64 start; u64 end; u64 page_flags; };
    const Section sections[] = {
        { "text",   reinterpret_cast<u64>(__text_start),   reinterpret_cast<u64>(__text_end),
          flags::kPresent },
        { "rodata", reinterpret_cast<u64>(__rodata_start), reinterpret_cast<u64>(__rodata_end),
          flags::kPresent | flags::kNoExecute },
        { "data",   reinterpret_cast<u64>(__data_start),   reinterpret_cast<u64>(__kernel_end),
          flags::kPresent | flags::kWrite | flags::kNoExecute },
    };

    for (const auto& section : sections) {
        const u64 start = mm::align_down(section.start, mm::kPageSize);
        const u64 end   = mm::align_up(section.end, mm::kPageSize);
        const u64 physical = physical_base + (start - virtual_base);

        if (!g_kernel_space.map_range(start, physical, end - start, section.page_flags))
            panic("vmm: failed to map kernel section %s", section.name);

        LOG_DEBUG("vmm: %-6s %#lx-%#lx -> phys %#lx%s%s",
                  section.name, start, end, physical,
                  (section.page_flags & flags::kWrite) ? " rw" : " ro",
                  (section.page_flags & flags::kNoExecute) ? " nx" : " x");
    }

    LOG_INFO("vmm: direct map covers %lu MiB, %lu page tables allocated",
             mapped_bytes / (1024 * 1024), g_stats.page_tables_allocated);

    // The point of no return: from here the bootloader's page tables are no
    // longer in use. The stack, the Limine responses and the kernel image are
    // all reachable through the mappings just built, or this faults instantly.
    g_kernel_space.activate();
    LOG_INFO("vmm: switched to kernel page tables at %#lx", g_kernel_space.root());
}

void init_ap() { g_kernel_space.activate(); }

bool handle_page_fault(const Registers* regs) {
    const u64 fault_address = cpu::read_cr2();
    const u64 error_code = regs->error_code;

    constexpr u64 kErrorPresent = 1 << 0;
    constexpr u64 kErrorWrite   = 1 << 1;
    constexpr u64 kErrorUser    = 1 << 2;

    AddressSpace* active = percpu::current()->address_space;
    if (active == nullptr) return false;

    // User code never resolves a fault on a kernel address, however the entry
    // happens to be marked: the kernel half is not its to page in.
    const bool user = (error_code & kErrorUser) != 0;
    if (user && mm::is_higher_half(fault_address)) return false;

    u64* entry = active->entry_for(fault_address, false);
    if (entry == nullptr) return false;

    // Nor does it reach a page that was never mapped for it.
    if (user && !(*entry & flags::kUser)) return false;

    // First touch of a reserved address: back it with a zeroed page now.
    if (!(error_code & kErrorPresent) && (*entry & flags::kDemand)) {
        const u64 page = pmm::alloc_page_zeroed();
        if (page == 0) return false;

        *entry = (*entry & ~flags::kDemand) | page | flags::kPresent;
        invalidate(fault_address);
        __atomic_fetch_add(&g_stats.demand_faults, 1, __ATOMIC_RELAXED);
        return true;
    }

    // Write to a shared page: give this space a private copy.
    if ((error_code & kErrorWrite) && (*entry & flags::kCopyOnWrite)) {
        const u64 old_physical = *entry & flags::kAddressMask;

        if (pmm::refcount(old_physical) == 1) {
            // Last reference -- no copy needed, just restore write access.
            *entry = (*entry & ~flags::kCopyOnWrite) | flags::kWrite;
        } else {
            const u64 new_physical = pmm::alloc_page();
            if (new_physical == 0) return false;

            memcpy(mm::phys_to_virt<void>(new_physical),
                   mm::phys_to_virt<void>(old_physical), mm::kPageSize);
            pmm::decref(old_physical);

            *entry = (*entry & ~(flags::kCopyOnWrite | flags::kAddressMask)) |
                     new_physical | flags::kWrite;
        }

        invalidate(fault_address);
        __atomic_fetch_add(&g_stats.cow_faults, 1, __ATOMIC_RELAXED);
        return true;
    }

    return false;
}

} // namespace vmm
