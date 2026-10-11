#pragma once
#include <mm/memory.hpp>

// Pure address arithmetic for x86-64 paging and the buddy allocator.
//
// Kept free of hardware access on purpose: this is the part of the memory
// subsystem that can be tested on the host, where a wrong shift shows up as a
// failed assertion instead of a triple fault.
namespace mm {

// Index into the page table at `level` (4 = PML4, 3 = PDPT, 2 = PD, 1 = PT).
constexpr u64 page_table_index(u64 virtual_address, int level) {
    return (virtual_address >> (12 + 9 * (level - 1))) & 0x1ff;
}

// x86-64 requires bits 63:47 to all equal bit 47. A non-canonical address
// faults as #GP, not #PF -- the CPU rejects the address before any page walk.
constexpr bool is_canonical(u64 virtual_address) {
    const u64 sign_extended = static_cast<u64>(static_cast<i64>(virtual_address << 16) >> 16);
    return sign_extended == virtual_address;
}

constexpr bool is_higher_half(u64 virtual_address) {
    return (virtual_address >> 47) != 0;
}

constexpr u64 kLargePageSize = 2 * 1024 * 1024;

constexpr u64 large_page_base(u64 virtual_address)   { return virtual_address & ~(kLargePageSize - 1); }
constexpr u64 large_page_offset(u64 virtual_address) { return virtual_address & (kLargePageSize - 1); }

// The buddy of a block is its index with the order's bit flipped.
constexpr u64 buddy_index(u64 index, u8 order) { return index ^ (1ULL << order); }

// Smallest buddy order that can hold `pages` pages.
constexpr u8 order_for_pages(u64 pages) {
    u8 order = 0;
    while ((1ULL << order) < pages) order++;
    return order;
}

} // namespace mm
