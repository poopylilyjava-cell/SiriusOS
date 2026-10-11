// Host-side tests for the pure address arithmetic shared by the PMM and VMM.
// A wrong shift here is a triple fault on real hardware; here it is a failed
// assertion with a debugger attached.

#include <cassert>
#include <cstdio>
#include <mm/paging_math.hpp>

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

int main() {
    using namespace mm;

    printf("page table indices:\n");
    // The kernel base, 0xffffffff80000000, must land in the last PML4 slot --
    // this is what -mcmodel=kernel requires and what the linker script assumes.
    constexpr u64 kernel_base = 0xffffffff80000000ULL;
    expect(page_table_index(kernel_base, 4) == 511, "kernel base is in PML4 slot 511");
    expect(page_table_index(kernel_base, 3) == 510, "kernel base is in PDPT slot 510");
    expect(page_table_index(kernel_base, 2) == 0,   "kernel base is in PD slot 0");
    expect(page_table_index(kernel_base, 1) == 0,   "kernel base is in PT slot 0");

    // The direct map base must be in the lower part of the kernel half.
    constexpr u64 hhdm_base = 0xffff800000000000ULL;
    expect(page_table_index(hhdm_base, 4) == 256, "the direct map starts at PML4 slot 256");

    // Slot 256 is the boundary: everything at or above it is the shared kernel
    // half that every address space copies.
    expect(page_table_index(0x0000'7fff'ffff'ffffULL, 4) == 255, "the user half ends at PML4 slot 255");

    // The selftest addresses must live in per-space lower-half tables, or the
    // copy-on-write test would be modifying tables shared with every space.
    expect(page_table_index(0x40000000ULL, 4) == 0, "the demand-paging test address is lower half");
    expect(page_table_index(0x50000000ULL, 4) == 0, "the copy-on-write test address is lower half");
    expect(page_table_index(0x60000000ULL, 4) == 0, "the read-only sharing test address is lower half");

    // Kernel stacks get a PML4 slot of their own in the shared kernel half,
    // away from the direct map (256) and the kernel image (511).
    expect(page_table_index(0xffffc00000000000ULL, 4) == 384, "kernel stacks live in PML4 slot 384");

    // Each level steps by 9 bits.
    expect(page_table_index(1ULL << 12, 1) == 1, "bit 12 selects PT slot 1");
    expect(page_table_index(1ULL << 21, 2) == 1, "bit 21 selects PD slot 1");
    expect(page_table_index(1ULL << 30, 3) == 1, "bit 30 selects PDPT slot 1");
    expect(page_table_index(1ULL << 39, 4) == 1, "bit 39 selects PML4 slot 1");

    printf("canonical addresses:\n");
    expect(is_canonical(0), "zero is canonical");
    expect(is_canonical(kernel_base), "the kernel base is canonical");
    expect(is_canonical(hhdm_base), "the direct map base is canonical");
    expect(is_canonical(0x0000'7fff'ffff'ffffULL), "the top of the user half is canonical");
    expect(is_canonical(0xffff'8000'0000'0000ULL), "the bottom of the kernel half is canonical");
    // This is the address that produced a #GP instead of a #PF in M0: bit 47 is
    // set but bits 63:48 are clear.
    expect(!is_canonical(0x0000'dead'0000'0000ULL), "bit 47 set with a zero prefix is not canonical");
    expect(!is_canonical(0x0000'8000'0000'0000ULL), "the first address above the user half is not canonical");
    expect(is_canonical(0x0000'0000'dead'0000ULL), "a plain lower-half address is canonical");

    expect(!is_higher_half(0x40000000ULL), "a low address is not in the higher half");
    expect(is_higher_half(kernel_base), "the kernel base is in the higher half");

    printf("large pages:\n");
    expect(large_page_base(0x200000ULL + 0x1234) == 0x200000ULL, "large page base masks the offset");
    expect(large_page_offset(0x200000ULL + 0x1234) == 0x1234, "large page offset is preserved");
    expect(large_page_base(kLargePageSize - 1) == 0, "an address below the first boundary has base 0");

    printf("buddy arithmetic:\n");
    // Buddies are symmetric, and merging a pair yields the lower of the two.
    expect(buddy_index(0, 0) == 1, "the buddy of frame 0 at order 0 is frame 1");
    expect(buddy_index(1, 0) == 0, "buddy is symmetric");
    expect(buddy_index(2, 1) == 0, "order 1 flips bit 1");
    expect(buddy_index(4, 2) == 0, "order 2 flips bit 2");
    for (u64 index = 0; index < 64; index++)
        for (u8 order = 0; order < 6; order++)
            expect(buddy_index(buddy_index(index, order), order) == index,
                   "taking the buddy twice returns the original block");

    printf("order sizing:\n");
    expect(order_for_pages(1) == 0, "one page is order 0");
    expect(order_for_pages(2) == 1, "two pages are order 1");
    expect(order_for_pages(3) == 2, "three pages round up to order 2");
    expect(order_for_pages(4) == 2, "four pages are order 2");
    expect(order_for_pages(1024) == 10, "1024 pages are order 10");

    printf("alignment:\n");
    expect(align_up(0, 4096) == 0, "zero is already aligned");
    expect(align_up(1, 4096) == 4096, "one byte rounds up to a page");
    expect(align_up(4096, 4096) == 4096, "an aligned value is unchanged");
    expect(align_down(4097, 4096) == 4096, "align_down truncates");
    expect(pages_for(0) == 0, "zero bytes need no pages");
    expect(pages_for(1) == 1, "one byte needs one page");
    expect(pages_for(4096) == 1, "a full page needs one page");
    expect(pages_for(4097) == 2, "one byte over needs two pages");
    // The size that exposed the physical-page-zero bug in the M1 stress test.
    expect(pages_for(2563) == 1, "2563 bytes fit in one page");

    if (g_failures != 0) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nall paging math tests passed\n");
    return 0;
}
