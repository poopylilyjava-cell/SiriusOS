#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <mm/paging_math.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <klib/spinlock.hpp>

namespace heap {
namespace {

struct Cache;

// Slab metadata lives at the front of the slab itself. Every page of a slab
// records the owning cache in its Frame, and buddy blocks of order N are
// 2^N-page aligned, so a pointer can be walked back to its slab header by
// masking -- no separate lookup structure.
struct SlabHeader {
    Cache*      cache;
    SlabHeader* next;
    SlabHeader* previous;
    void*       free_list;   // singly linked through the free objects themselves
    u32         in_use;
    u32         capacity;
};

struct Cache {
    u32         object_size;
    u8          slab_order;
    u32         capacity;       // objects per slab
    u32         first_offset;
    SlabHeader* partial;        // slabs with at least one free object
    u64         live_objects;
};

// Power-of-two size classes. Larger classes use bigger slabs so the per-slab
// header stays a small fraction of the whole.
constexpr u32 kSizeClasses[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
constexpr usize kCacheCount = sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);
constexpr u32 kFirstObjectAlign = 64;  // keep objects off the header's cache line

Cache g_caches[kCacheCount];
Stats g_stats{};
bool  g_initialised = false;

// Guards every cache and the counters. Taken before the page allocator's lock
// when a slab grows or shrinks, never after it.
Spinlock g_lock;

u8 slab_order_for(u32 object_size) {
    if (object_size <= 512)  return 0;  // 4 KiB
    if (object_size <= 1024) return 1;  // 8 KiB
    return 2;                           // 16 KiB
}

u64 slab_bytes(const Cache& cache) { return mm::kPageSize << cache.slab_order; }

Cache* cache_for_size(usize size) {
    for (usize i = 0; i < kCacheCount; i++)
        if (size <= kSizeClasses[i]) return &g_caches[i];
    return nullptr;
}

SlabHeader* slab_of(const void* pointer, const Cache& cache) {
    return reinterpret_cast<SlabHeader*>(
        mm::align_down(reinterpret_cast<u64>(pointer), slab_bytes(cache)));
}

void partial_push(Cache& cache, SlabHeader* slab) {
    slab->previous = nullptr;
    slab->next = cache.partial;
    if (cache.partial) cache.partial->previous = slab;
    cache.partial = slab;
}

void partial_remove(Cache& cache, SlabHeader* slab) {
    if (slab->previous) slab->previous->next = slab->next;
    else                cache.partial = slab->next;
    if (slab->next) slab->next->previous = slab->previous;
    slab->next = slab->previous = nullptr;
}

SlabHeader* grow(Cache& cache) {
    const u64 physical = pmm::alloc_pages(cache.slab_order);
    if (physical == 0) return nullptr;

    auto* slab = mm::phys_to_virt<SlabHeader>(physical);
    slab->cache     = &cache;
    slab->next      = nullptr;
    slab->previous  = nullptr;
    slab->free_list = nullptr;
    slab->in_use    = 0;
    slab->capacity  = cache.capacity;

    // Thread every object onto the free list, back to front so allocation
    // hands them out in ascending address order.
    auto* base = reinterpret_cast<u8*>(slab) + cache.first_offset;
    for (u32 i = cache.capacity; i > 0; i--) {
        void* object = base + static_cast<u64>(i - 1) * cache.object_size;
        *reinterpret_cast<void**>(object) = slab->free_list;
        slab->free_list = object;
    }

    // Tag every page of the slab, so free() can identify slab memory from any
    // pointer inside it.
    for (u64 page = 0; page < (1ULL << cache.slab_order); page++)
        pmm::frame_for(physical + page * mm::kPageSize)->owner = &cache;

    partial_push(cache, slab);
    g_stats.slabs_allocated++;
    return slab;
}

void shrink(Cache& cache, SlabHeader* slab) {
    partial_remove(cache, slab);

    const u64 physical = mm::virt_to_phys(slab);
    for (u64 page = 0; page < (1ULL << cache.slab_order); page++)
        pmm::frame_for(physical + page * mm::kPageSize)->owner = nullptr;

    pmm::free_pages(physical, cache.slab_order);
    g_stats.slabs_released++;
}

} // namespace

void init() {
    for (usize i = 0; i < kCacheCount; i++) {
        Cache& cache = g_caches[i];
        cache.object_size  = kSizeClasses[i];
        cache.slab_order   = slab_order_for(cache.object_size);
        cache.first_offset = static_cast<u32>(mm::align_up(sizeof(SlabHeader), kFirstObjectAlign));
        cache.capacity     = static_cast<u32>((slab_bytes(cache) - cache.first_offset) / cache.object_size);
        cache.partial      = nullptr;
        cache.live_objects = 0;

        if (cache.capacity == 0) panic("heap: size class %u does not fit in its slab", cache.object_size);
    }
    g_initialised = true;

    LOG_INFO("heap: %lu slab caches, %u..%u bytes", kCacheCount,
             kSizeClasses[0], kSizeClasses[kCacheCount - 1]);
}

void* allocate(usize size) {
    if (!g_initialised) panic("heap: allocation before heap::init()");
    if (size == 0) return nullptr;
    ScopedLock guard(g_lock);

    if (size > kMaxSlabObject) {
        // Round up to a buddy order. The Frame's own order records the size,
        // so free() needs no header of ours.
        const u8 order = mm::order_for_pages(mm::pages_for(size));

        const u64 physical = pmm::alloc_pages(order);
        if (physical == 0) return nullptr;
        pmm::frame_for(physical)->owner = nullptr;

        g_stats.live_objects++;
        g_stats.live_bytes += mm::kPageSize << order;
        g_stats.total_allocations++;
        g_stats.large_allocations++;
        return mm::phys_to_virt<void>(physical);
    }

    Cache* cache = cache_for_size(size);
    SlabHeader* slab = cache->partial;
    if (slab == nullptr) {
        slab = grow(*cache);
        if (slab == nullptr) return nullptr;
    }

    void* object = slab->free_list;
    slab->free_list = *reinterpret_cast<void**>(object);
    slab->in_use++;

    // A full slab leaves the partial list; free() puts it back.
    if (slab->free_list == nullptr) partial_remove(*cache, slab);

    cache->live_objects++;
    g_stats.live_objects++;
    g_stats.live_bytes += cache->object_size;
    g_stats.total_allocations++;
    return object;
}

void free(void* pointer) {
    if (pointer == nullptr) return;
    ScopedLock guard(g_lock);

    const u64 physical = mm::virt_to_phys(pointer);
    pmm::Frame* frame = pmm::frame_for(physical);
    if (frame == nullptr) panic("heap: free of a pointer outside physical memory: %p", pointer);

    if (frame->owner == nullptr) {
        // A whole-pages allocation.
        if ((reinterpret_cast<u64>(pointer) & (mm::kPageSize - 1)) != 0)
            panic("heap: free of unaligned large allocation %p", pointer);

        const u8 order = frame->order;
        g_stats.live_objects--;
        g_stats.live_bytes -= mm::kPageSize << order;
        g_stats.total_frees++;
        pmm::free_pages(physical, order);
        return;
    }

    auto* cache = static_cast<Cache*>(frame->owner);
    SlabHeader* slab = slab_of(pointer, *cache);
    if (slab->cache != cache) panic("heap: slab header at %p is corrupt", slab);
    if (slab->in_use == 0) panic("heap: free of %p into an empty slab (double free?)", pointer);

    const bool was_full = (slab->free_list == nullptr);

    *reinterpret_cast<void**>(pointer) = slab->free_list;
    slab->free_list = pointer;
    slab->in_use--;

    cache->live_objects--;
    g_stats.live_objects--;
    g_stats.live_bytes -= cache->object_size;
    g_stats.total_frees++;

    if (was_full) partial_push(*cache, slab);
    // Return fully drained slabs so a burst of allocations does not permanently
    // pin memory in one size class.
    if (slab->in_use == 0) shrink(*cache, slab);
}

usize allocated_size(const void* pointer) {
    if (pointer == nullptr) return 0;
    pmm::Frame* frame = pmm::frame_for(mm::virt_to_phys(pointer));
    if (frame == nullptr) return 0;
    if (frame->owner == nullptr) return mm::kPageSize << frame->order;
    return static_cast<Cache*>(frame->owner)->object_size;
}

Stats stats() {
    ScopedLock guard(g_lock);
    return g_stats;
}

void dump_stats(const char* context) {
    const Stats snapshot = stats();
    LOG_INFO("heap[%s]: %lu live objects (%lu KiB), %lu allocs, %lu frees, slabs +%lu -%lu",
             context, snapshot.live_objects, snapshot.live_bytes / 1024,
             snapshot.total_allocations, snapshot.total_frees,
             snapshot.slabs_allocated, snapshot.slabs_released);
}

} // namespace heap

void* kmalloc(usize size) { return heap::allocate(size); }

void* kzalloc(usize size) {
    void* pointer = heap::allocate(size);
    if (pointer != nullptr) memset(pointer, 0, size);
    return pointer;
}

void kfree(void* pointer) { heap::free(pointer); }
