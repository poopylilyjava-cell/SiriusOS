// In-kernel tests for the memory subsystem.
//
// These have to run on the real machine: the buddy allocator, the page tables
// and the fault handlers only exist against a real CR3 and a real MMU. The
// pure formatting and string code is tested on the host instead.
#include <mm/selftest.hpp>
#include <mm/pmm.hpp>
#include <mm/vmm.hpp>
#include <mm/heap.hpp>
#include <mm/paging_math.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>

namespace mm {
namespace {

u32 g_checks = 0;
u32 g_failures = 0;

void check(bool condition, const char* what) {
    g_checks++;
    if (condition) return;
    g_failures++;
    LOG_ERROR("selftest FAILED: %s", what);
}

// xorshift64*: small, fast, and good enough to make the stress test's access
// pattern unpredictable.
u64 g_random_state = 0x2545f4914f6cdd1dULL;
u64 next_random() {
    g_random_state ^= g_random_state >> 12;
    g_random_state ^= g_random_state << 25;
    g_random_state ^= g_random_state >> 27;
    return g_random_state * 0x2545f4914f6cdd1dULL;
}
u64 random_below(u64 bound) { return next_random() % bound; }

void test_pmm_basics() {
    LOG_INFO("selftest: pmm basics");
    const u64 free_before = pmm::stats().free_pages;

    const u64 a = pmm::alloc_page();
    const u64 b = pmm::alloc_page();
    check(a != 0 && b != 0, "single page allocation succeeds");
    check(a != b, "distinct allocations return distinct pages");
    check((a % kPageSize) == 0, "allocated pages are page aligned");

    // The direct map must actually reach the page we were handed.
    auto* scratch = phys_to_virt<u64>(a);
    *scratch = 0xfeedfacecafebeefULL;
    check(*scratch == 0xfeedfacecafebeefULL, "allocated page is readable and writable");

    pmm::free_page(a);
    pmm::free_page(b);
    check(pmm::stats().free_pages == free_before, "freeing restores the free page count");

    // Higher orders must be contiguous and correctly aligned.
    const u64 block = pmm::alloc_pages(4);   // 16 pages
    check(block != 0, "order-4 allocation succeeds");
    check((block % (kPageSize * 16)) == 0, "buddy blocks are aligned to their size");

    auto* span = phys_to_virt<u8>(block);
    memset(span, 0xa5, kPageSize * 16);
    check(span[kPageSize * 16 - 1] == 0xa5, "the whole order-4 block is addressable");

    pmm::free_pages(block, 4);
    check(pmm::stats().free_pages == free_before, "order-4 block returns to the pool");
}

void test_pmm_split_and_coalesce() {
    LOG_INFO("selftest: pmm split and coalesce");
    const u64 free_before = pmm::stats().free_pages;

    // Taking a small block forces a large one to split; releasing everything
    // must merge it back, or the allocator fragments permanently.
    u64 pages[16];
    for (auto& page : pages) page = pmm::alloc_page();
    for (auto page : pages) check(page != 0, "split allocations all succeed");
    for (auto page : pages) pmm::free_page(page);

    check(pmm::stats().free_pages == free_before, "split blocks coalesce back");

    const u64 large = pmm::alloc_pages(pmm::kMaxOrder);
    check(large != 0, "a maximum-order block is available after coalescing");
    if (large != 0) pmm::free_pages(large, pmm::kMaxOrder);
    check(pmm::stats().free_pages == free_before, "maximum-order block returns to the pool");
}

void test_pmm_refcounts() {
    LOG_INFO("selftest: pmm refcounts");
    const u64 free_before = pmm::stats().free_pages;

    const u64 page = pmm::alloc_page();
    check(pmm::refcount(page) == 1, "a fresh page has one reference");

    pmm::incref(page);
    check(pmm::refcount(page) == 2, "incref raises the count");
    check(pmm::decref(page) == 1, "decref returns the remaining count");
    check(pmm::stats().free_pages == free_before - 1, "a referenced page stays allocated");

    check(pmm::decref(page) == 0, "the last decref drops to zero");
    check(pmm::stats().free_pages == free_before, "the page is freed when unreferenced");
}

void test_vmm_mapping() {
    LOG_INFO("selftest: vmm mapping");
    constexpr u64 kTestAddress = 0xffffa00000000000ULL;
    auto& space = vmm::kernel_space();

    const u64 physical = pmm::alloc_page_zeroed();
    check(space.map(kTestAddress, physical,
                    vmm::flags::kPresent | vmm::flags::kWrite | vmm::flags::kNoExecute),
          "mapping a fresh virtual address succeeds");

    check(space.translate(kTestAddress) == physical, "translate returns the mapped frame");

    auto* through_mapping = reinterpret_cast<volatile u64*>(kTestAddress);
    *through_mapping = 0x0123456789abcdefULL;

    // The same physical page seen through the direct map must show the write.
    check(*phys_to_virt<u64>(physical) == 0x0123456789abcdefULL,
          "writes through a new mapping reach the underlying page");

    // An offset within the page must translate to the same offset.
    check(space.translate(kTestAddress + 0x40) == physical + 0x40,
          "translate preserves the offset within a page");

    check(space.unmap(kTestAddress), "unmapping succeeds");
    check(space.translate(kTestAddress) == 0, "an unmapped address no longer translates");

    pmm::free_page(physical);
}

void test_vmm_demand_paging() {
    LOG_INFO("selftest: vmm demand paging");
    // A lower-half address: those page tables belong to this address space
    // alone, unlike the shared kernel half.
    constexpr u64 kTestAddress = 0x40000000ULL;
    auto& space = vmm::kernel_space();

    const u64 faults_before = vmm::stats().demand_faults;

    check(space.map_demand(kTestAddress, vmm::flags::kPresent | vmm::flags::kWrite),
          "reserving a demand-paged address succeeds");
    check(space.translate(kTestAddress) == 0, "a reserved address has no page yet");

    // Touching it must fault, allocate, and resume -- if the handler is wrong
    // this panics instead of returning.
    auto* page = reinterpret_cast<volatile u64*>(kTestAddress);
    check(*page == 0, "a demand-paged page reads as zero");
    *page = 0xdeadbeefULL;
    check(*page == 0xdeadbeefULL, "a demand-paged page is writable after the fault");

    check(vmm::stats().demand_faults == faults_before + 1, "exactly one demand fault was taken");

    const u64 backing = space.translate(kTestAddress);
    check(backing != 0, "the demand-paged address is now backed");

    space.unmap(kTestAddress);
    pmm::free_page(backing);
}

void test_vmm_copy_on_write() {
    LOG_INFO("selftest: vmm copy-on-write");
    constexpr u64 kTestAddress = 0x50000000ULL;
    constexpr u64 kOriginal = 0x1111111111111111ULL;
    constexpr u64 kModified = 0x2222222222222222ULL;

    auto& kernel = vmm::kernel_space();
    const u64 cow_before = vmm::stats().cow_faults;

    const u64 shared = pmm::alloc_page_zeroed();
    check(kernel.map(kTestAddress, shared, vmm::flags::kPresent | vmm::flags::kWrite),
          "mapping the page to be shared succeeds");

    auto* through_kernel = reinterpret_cast<volatile u64*>(kTestAddress);
    *through_kernel = kOriginal;

    vmm::AddressSpace child;
    check(child.create(), "creating a second address space succeeds");
    check(kernel.share_copy_on_write(child, kTestAddress), "sharing the page copy-on-write succeeds");
    check(pmm::refcount(shared) == 2, "the shared page has two references");

    // Switch to the child and write: this must fault, copy, and resume.
    child.activate();
    auto* through_child = reinterpret_cast<volatile u64*>(kTestAddress);
    check(*through_child == kOriginal, "the child initially sees the shared contents");
    *through_child = kModified;
    check(*through_child == kModified, "the child's write lands after the copy");

    const u64 child_page = child.translate(kTestAddress);
    kernel.activate();

    check(vmm::stats().cow_faults == cow_before + 1, "exactly one copy-on-write fault was taken");
    check(child_page != shared, "the child was given a private copy");
    check(pmm::refcount(shared) == 1, "the original page dropped back to one reference");
    check(*through_kernel == kOriginal, "the parent's copy is unchanged");

    // The parent still holds a copy-on-write marker; writing must now simply
    // reclaim write access rather than copying again, since it is the last ref.
    *through_kernel = kModified;
    check(*through_kernel == kModified, "the last reference regains write access");
    check(kernel.translate(kTestAddress) == shared, "no needless copy was made");

    kernel.unmap(kTestAddress);
    pmm::free_page(shared);

    // The child's private copy is still mapped: tearing the space down must
    // release it, or every exiting process would leak its pages.
    const u64 free_before_destroy = pmm::stats().free_pages;
    child.destroy();
    check(pmm::refcount(child_page) == 0, "destroy releases pages still mapped in the space");
    check(pmm::stats().free_pages > free_before_destroy, "destroy returns the copy and its tables");
}

// A page that was never writable must stay that way when shared: marking it
// copy-on-write would let the fault handler hand out write access to it.
void test_vmm_share_read_only() {
    LOG_INFO("selftest: vmm read-only sharing");
    constexpr u64 kTestAddress = 0x60000000ULL;
    auto& kernel = vmm::kernel_space();

    const u64 page = pmm::alloc_page_zeroed();
    check(kernel.map(kTestAddress, page, vmm::flags::kPresent | vmm::flags::kNoExecute),
          "mapping a read-only page succeeds");

    vmm::AddressSpace child;
    check(child.create(), "creating a second address space succeeds");
    check(kernel.share_copy_on_write(child, kTestAddress), "sharing a read-only page succeeds");
    check(pmm::refcount(page) == 2, "the read-only page has two references");

    const u64* parent_entry = kernel.entry_for(kTestAddress, false);
    const u64* child_entry  = child.entry_for(kTestAddress, false);
    const u64 kWritableBits = vmm::flags::kWrite | vmm::flags::kCopyOnWrite;
    check(parent_entry && !(*parent_entry & kWritableBits),
          "the parent's read-only page gains no copy-on-write marker");
    check(child_entry && !(*child_entry & kWritableBits),
          "the child's view is read-only with no copy-on-write marker");
    check(child.translate(kTestAddress) == page, "the child maps the same frame");

    child.unmap(kTestAddress);
    kernel.unmap(kTestAddress);
    pmm::decref(page);
    pmm::decref(page);
    child.destroy();
}

// A user leaf is only reachable if every table above it is user-accessible
// too: the CPU ands the permissions along the walk.
void test_vmm_user_intermediates() {
    LOG_INFO("selftest: vmm user intermediates");
    constexpr u64 kTestAddress = 0x70000000ULL;
    const u64 free_before = pmm::stats().free_pages;

    // A fresh space, so every lower-half table on the walk is created here.
    vmm::AddressSpace space;
    check(space.create(), "creating an address space succeeds");
    check(space.map_demand(kTestAddress,
                           vmm::flags::kPresent | vmm::flags::kWrite | vmm::flags::kUser),
          "reserving a user demand page succeeds");

    const u64* table = phys_to_virt<u64>(space.root());
    bool all_user = true;
    for (int level = 4; level > 1; level--) {
        const u64 entry = table[page_table_index(kTestAddress, level)];
        if (!(entry & vmm::flags::kUser)) all_user = false;
        table = phys_to_virt<u64>(entry & vmm::flags::kAddressMask);
    }
    check(all_user, "tables above a user demand page are user-accessible");

    space.destroy();
    check(pmm::stats().free_pages == free_before, "destroying the space returns its tables");
}

// The M1 exit test: 100k allocations of random size across 1000 iterations,
// with the live-object counter required to return to zero.
void test_heap_stress() {
    LOG_INFO("selftest: heap stress (100k operations)");

    constexpr usize kSlots = 1024;
    constexpr u32 kIterations = 1000;
    constexpr u32 kOperationsPerIteration = 100;

    struct Slot { void* pointer; usize size; u8 pattern; };
    static Slot slots[kSlots];
    memset(slots, 0, sizeof(slots));

    const heap::Stats heap_before = heap::stats();
    const u64 pmm_free_before = pmm::stats().free_pages;

    u64 allocations = 0, frees = 0, failed = 0, corruption = 0;

    for (u32 iteration = 0; iteration < kIterations; iteration++) {
        for (u32 operation = 0; operation < kOperationsPerIteration; operation++) {
            const usize index = random_below(kSlots);
            Slot& slot = slots[index];

            if (slot.pointer != nullptr) {
                // Verify the payload before releasing it: if the allocator ever
                // hands the same memory out twice, the pattern will not match.
                const auto* bytes = static_cast<const u8*>(slot.pointer);
                for (usize i = 0; i < slot.size; i++) {
                    if (bytes[i] != slot.pattern) { corruption++; break; }
                }
                kfree(slot.pointer);
                slot.pointer = nullptr;
                frees++;
                continue;
            }

            // Sizes span both slab classes and the whole-pages path.
            const usize size = 1 + random_below(4096);
            void* pointer = kmalloc(size);
            if (pointer == nullptr) {
                failed++;
                LOG_ERROR("selftest: kmalloc(%lu) returned null at iteration %u (free pages %lu)",
                          size, iteration, pmm::stats().free_pages);
                continue;
            }

            slot.pointer = pointer;
            slot.size    = size;
            slot.pattern = static_cast<u8>(index ^ size);
            memset(pointer, slot.pattern, size);
            allocations++;
        }
    }

    // Drain whatever is still live.
    for (auto& slot : slots) {
        if (slot.pointer == nullptr) continue;
        kfree(slot.pointer);
        slot.pointer = nullptr;
        frees++;
    }

    const heap::Stats heap_after = heap::stats();

    LOG_INFO("selftest: %lu allocations, %lu frees, %lu failed, %lu corrupted",
             allocations, frees, failed, corruption);

    check(corruption == 0, "no allocation overlapped another");
    check(failed == 0, "no allocation failed");
    check(allocations == frees, "every allocation was freed");
    check(heap_after.live_objects == heap_before.live_objects, "heap live object count returns to baseline");
    check(heap_after.live_bytes == heap_before.live_bytes, "heap live byte count returns to baseline");
    check(pmm::stats().free_pages == pmm_free_before, "every slab page returns to the page allocator");
}

void test_heap_edge_cases() {
    LOG_INFO("selftest: heap edge cases");
    const u64 pmm_free_before = pmm::stats().free_pages;

    check(kmalloc(0) == nullptr, "a zero-size allocation returns null");
    kfree(nullptr);  // must be a no-op

    // Every size class boundary, plus the first size that skips the slabs.
    const usize sizes[] = { 1, 16, 17, 32, 256, 512, 1024, 2048, 2049, 4096, 8192, 100000 };
    for (usize size : sizes) {
        void* pointer = kmalloc(size);
        check(pointer != nullptr, "allocation at a size-class boundary succeeds");
        check(heap::allocated_size(pointer) >= size, "the served size class fits the request");
        check((reinterpret_cast<u64>(pointer) & 0xf) == 0, "allocations are at least 16-byte aligned");
        memset(pointer, 0x5a, size);
        kfree(pointer);
    }

    void* zeroed = kzalloc(777);
    check(zeroed != nullptr, "kzalloc succeeds");
    const auto* bytes = static_cast<const u8*>(zeroed);
    bool all_zero = true;
    for (usize i = 0; i < 777; i++) if (bytes[i] != 0) { all_zero = false; break; }
    check(all_zero, "kzalloc returns zeroed memory");
    kfree(zeroed);

    // new/delete route through the same heap.
    struct Probe { u64 a, b; };
    auto* probe = new Probe{1, 2};
    check(probe != nullptr && probe->a == 1 && probe->b == 2, "operator new works");
    delete probe;

    check(pmm::stats().free_pages == pmm_free_before, "edge cases leak no pages");
}

} // namespace

bool run_selftests() {
    LOG_INFO("selftest: starting memory tests");
    pmm::dump_stats("before");
    heap::dump_stats("before");

    const u64 pmm_free_at_start = pmm::stats().free_pages;

    test_pmm_basics();
    test_pmm_split_and_coalesce();
    test_pmm_refcounts();
    test_vmm_mapping();
    test_vmm_demand_paging();
    test_vmm_copy_on_write();
    test_vmm_share_read_only();
    test_vmm_user_intermediates();
    test_heap_edge_cases();
    test_heap_stress();

    check(pmm::stats().free_pages == pmm_free_at_start,
          "the whole test run leaks no physical pages");

    pmm::dump_stats("after");
    heap::dump_stats("after");

    const auto vmm_stats = vmm::stats();
    LOG_INFO("selftest: %lu demand faults, %lu copy-on-write faults",
             vmm_stats.demand_faults, vmm_stats.cow_faults);

    if (g_failures == 0) {
        LOG_INFO("selftest: all %u checks passed", g_checks);
        return true;
    }
    LOG_ERROR("selftest: %u of %u checks FAILED", g_failures, g_checks);
    return false;
}

} // namespace mm
