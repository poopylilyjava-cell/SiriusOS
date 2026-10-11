#pragma once
#include <mm/memory.hpp>

namespace heap {

// Objects up to this size come from slab caches; anything larger is rounded up
// to whole pages and served by the buddy allocator directly.
constexpr usize kMaxSlabObject = 2048;

struct Stats {
    u64 live_objects;   // outstanding allocations -- must return to 0 in tests
    u64 live_bytes;
    u64 total_allocations;
    u64 total_frees;
    u64 slabs_allocated;
    u64 slabs_released;
    u64 large_allocations;
};

void init();

void* allocate(usize size);
void  free(void* pointer);

// Size class this allocation was actually served from, for accounting.
usize allocated_size(const void* pointer);

Stats stats();
void  dump_stats(const char* context);

} // namespace heap

void* kmalloc(usize size);
void* kzalloc(usize size);
void  kfree(void* pointer);
