#pragma once
#include <klib/types.hpp>

namespace mm {

constexpr u64 kPageSize  = 4096;
constexpr u64 kPageShift = 12;

constexpr u64 align_up(u64 value, u64 alignment)   { return (value + alignment - 1) & ~(alignment - 1); }
constexpr u64 align_down(u64 value, u64 alignment) { return value & ~(alignment - 1); }
constexpr u64 pages_for(u64 bytes) { return align_up(bytes, kPageSize) / kPageSize; }

// The bootloader maps all usable physical memory at a fixed higher-half offset
// (the HHDM). Until the kernel builds its own page tables this is the only way
// to touch a physical address, and it stays the cheapest way afterwards.
void set_hhdm_offset(u64 offset);
u64  hhdm_offset();

template <typename T>
inline T* phys_to_virt(u64 physical) { return reinterpret_cast<T*>(physical + hhdm_offset()); }

inline u64 virt_to_phys(const void* virtual_address) {
    return reinterpret_cast<u64>(virtual_address) - hhdm_offset();
}

} // namespace mm
