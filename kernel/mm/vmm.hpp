#pragma once
#include <mm/memory.hpp>

struct limine_memmap_response;
struct Registers;

namespace vmm {

// x86-64 page table entry bits. Bits 9-11 are ignored by the hardware and free
// for software use, which is where the demand-paging and copy-on-write markers
// live.
namespace flags {
constexpr u64 kPresent      = 1ULL << 0;
constexpr u64 kWrite        = 1ULL << 1;
constexpr u64 kUser         = 1ULL << 2;
constexpr u64 kWriteThrough = 1ULL << 3;
constexpr u64 kNoCache      = 1ULL << 4;
constexpr u64 kAccessed     = 1ULL << 5;
constexpr u64 kDirty        = 1ULL << 6;
constexpr u64 kHuge         = 1ULL << 7;  // PS bit: 2 MiB page at PD level
constexpr u64 kGlobal       = 1ULL << 8;
constexpr u64 kDemand       = 1ULL << 9;  // software: allocate on first touch
constexpr u64 kCopyOnWrite  = 1ULL << 10; // software: copy on first write
constexpr u64 kDevice       = 1ULL << 11; // software: device memory, not a page of ours
constexpr u64 kShared       = 1ULL << 52; // software: a page of a shared memory object; fork shares it, writable
constexpr u64 kNoExecute    = 1ULL << 63;

constexpr u64 kPatLarge     = 1ULL << 12; // PAT bit for 2 MiB entries
constexpr u64 kPatSmall     = 1ULL << 7;  // PAT bit for 4 KiB entries

// Limine guarantees PAT5 = write-combining. Index 5 is PAT=1, PCD=0, PWT=1.
constexpr u64 kWriteCombiningLarge = kPatLarge | kWriteThrough;
constexpr u64 kWriteCombiningSmall = kPatSmall | kWriteThrough;

constexpr u64 kAddressMask = 0x000ffffffffff000ULL;
} // namespace flags

enum class MemoryType { Normal, WriteCombining };

class AddressSpace {
public:
    // Creates a fresh root that shares every kernel higher-half mapping, so a
    // kernel pointer means the same thing in every address space.
    //
    // An AddressSpace does no locking of its own. A user space belongs to one
    // process, whose threads all run on one CPU: changes to it are made
    // there, each with interrupts off, which keeps those threads and their
    // page faults out of the middle. The shared kernel half is changed only
    // through vmm functions that hold the kernel mapping lock.
    bool create();
    // Releases the lower half: one reference to every page still mapped there,
    // then the page tables themselves. Unlike unmap(), which leaves the page
    // with the caller, teardown is where an address space gives up its pages.
    void destroy();

    bool map(u64 virtual_address, u64 physical, u64 page_flags);
    bool map_range(u64 virtual_address, u64 physical, u64 size, u64 page_flags,
                   MemoryType type = MemoryType::Normal);
    bool unmap(u64 virtual_address);

    // Physical address backing this virtual address, or 0 if not mapped.
    u64 translate(u64 virtual_address) const;

    // Map one page of device memory (a framebuffer) for user space. Such a
    // page is never reference counted, copied or freed: fork shares it and
    // teardown just forgets it.
    bool map_device(u64 virtual_address, u64 physical, u64 page_flags, MemoryType type);

    // Map a page of a shared memory object. The caller passes on a reference
    // to the frame, which the mapping keeps until it is released. Unlike
    // every other writable page, it stays shared and writable across fork.
    bool map_shared(u64 virtual_address, u64 physical, u64 page_flags);

    // Reserve an address with no backing page; the first access faults it in.
    bool map_demand(u64 virtual_address, u64 page_flags);

    // Share a mapping into another space as copy-on-write in both.
    bool share_copy_on_write(AddressSpace& target, u64 virtual_address);

    // Give `child` (freshly created) the whole user half of this space:
    // mapped pages shared copy-on-write, demand reservations copied. This
    // space must be the one loaded on the calling CPU, since its write
    // permissions are revoked and the TLB flushed here. False if out of memory.
    bool fork_into(AddressSpace& child);

    // Unmap one page and drop the reference to its frame, freeing it if this
    // was the last. A demand reservation is simply removed.
    void release(u64 virtual_address);

    void activate() const;
    u64  root() const { return m_root; }
    bool valid() const { return m_root != 0; }

    // Entry for this address, optionally creating the intermediate tables.
    // `leaf_flags` are the flags the leaf will carry: a user leaf needs the
    // tables above it user-accessible too.
    u64* entry_for(u64 virtual_address, bool create_missing, u64 leaf_flags = 0);

private:
    u64 m_root = 0;  // physical address of the PML4
};

// Builds the kernel's own page tables and switches to them, taking ownership
// away from the bootloader.
void init(const limine_memmap_response* memmap);

// Load the kernel's page tables on an application processor, which arrives
// still running on the bootloader's.
void init_ap();

AddressSpace& kernel_space();

// Allocate a kernel stack of `pages` pages in its own virtual range, with an
// unmapped guard page on either side, and return its initial stack pointer
// (0 on failure). The direct map is contiguous, so a stack addressed through
// it overflows silently into whatever physical memory sits below; here the
// overflow hits the guard and faults.
u64 create_kernel_stack(u64 pages);

// Unmap and free a stack made by create_kernel_stack(). Its virtual range is
// not reused: other CPUs may still hold TLB entries for it, and there is no
// TLB shootdown yet. The range is 512 GiB, so this costs nothing real.
void destroy_kernel_stack(u64 stack_top, u64 pages);

// Map device registers uncached into a dedicated kernel range and return the
// virtual address of `physical` (which need not be page aligned), or 0.
u64 map_mmio(u64 physical, u64 size);

// Returns true if the fault was resolved and the instruction may be retried.
// Resolves against the address space loaded on the faulting CPU.
bool handle_page_fault(const Registers* regs);

void invalidate(u64 virtual_address);

struct Stats { u64 demand_faults; u64 cow_faults; u64 page_tables_allocated; u64 page_tables_freed; };
Stats stats();

} // namespace vmm
