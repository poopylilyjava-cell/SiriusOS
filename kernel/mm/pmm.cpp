#include <mm/pmm.hpp>
#include <mm/paging_math.hpp>
#include <klib/print.hpp>
#include <klib/panic.hpp>
#include <klib/string.hpp>
#include <klib/spinlock.hpp>
#include <limine.h>

namespace pmm {
namespace {

Frame* g_frames = nullptr;     // one entry per page frame, indexed by PFN
u64    g_frame_count = 0;      // frames covered, i.e. highest usable PFN + 1
Frame* g_free_lists[kMaxOrder + 1] = {};

Stats g_stats{};

// One lock for the whole allocator. The free lists, the frame records and the
// counters all change together; per-order locks would buy nothing until there
// is measurable contention.
Spinlock g_lock;

u64 pfn_of(u64 physical) { return physical >> mm::kPageShift; }

bool valid_pfn(u64 pfn) { return pfn < g_frame_count; }

void list_push(u8 order, Frame* frame) {
    frame->next = g_free_lists[order];
    frame->prev = nullptr;
    if (g_free_lists[order]) g_free_lists[order]->prev = frame;
    g_free_lists[order] = frame;
}

void list_remove(u8 order, Frame* frame) {
    if (frame->prev) frame->prev->next = frame->next;
    else             g_free_lists[order] = frame->next;
    if (frame->next) frame->next->prev = frame->prev;
    frame->next = frame->prev = nullptr;
}

// Two buddies can only merge if they are both free, both the same order, and
// both actually exist -- which is what keeps holes in the memory map from being
// merged across.
u64 buddy_pfn(u64 pfn, u8 order) { return mm::buddy_index(pfn, order); }

void mark_block_free(u64 pfn, u8 order) {
    Frame* frame = &g_frames[pfn];
    frame->order = order;
    frame->flags = kFrameFree | kFrameIsHead;
    frame->refcount = 0;
    frame->owner = nullptr;
    list_push(order, frame);
}

// Release a block, merging with its buddy as far up the orders as possible.
void release(u64 pfn, u8 order) {
    // Count what is being released, not what it merges into: after coalescing,
    // `order` describes a block whose other half was already counted as free.
    g_stats.free_pages += 1ULL << order;

    while (order < kMaxOrder) {
        const u64 buddy = buddy_pfn(pfn, order);
        if (!valid_pfn(buddy)) break;

        Frame* buddy_frame = &g_frames[buddy];
        if (!(buddy_frame->flags & kFrameFree)) break;
        if (buddy_frame->order != order) break;
        if (!(buddy_frame->flags & kFrameIsHead)) break;

        list_remove(order, buddy_frame);
        buddy_frame->flags = 0;

        // The merged block is headed by whichever of the two is lower.
        if (buddy < pfn) {
            g_frames[pfn].flags = 0;
            pfn = buddy;
        }
        order++;
    }
    mark_block_free(pfn, order);
}

} // namespace

void init(const limine_memmap_response* memmap) {
    if (memmap == nullptr) panic("pmm: bootloader supplied no memory map");

    // Size the frame array to cover every page up to the highest usable byte.
    // Reserved regions above that are never allocatable, so they need no record.
    u64 highest = 0;
    u64 usable_bytes = 0;
    for (u64 i = 0; i < memmap->entry_count; i++) {
        const auto* entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        usable_bytes += entry->length;
        const u64 end = entry->base + entry->length;
        if (end > highest) highest = end;
    }
    if (highest == 0) panic("pmm: no usable memory");

    g_frame_count = mm::pages_for(highest);
    const u64 array_bytes = mm::align_up(g_frame_count * sizeof(Frame), mm::kPageSize);

    // Bootstrap: the frame array itself must come from somewhere before the
    // allocator exists, so carve it out of the first usable region big enough.
    u64 array_physical = 0;
    for (u64 i = 0; i < memmap->entry_count; i++) {
        const auto* entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        if (entry->length < array_bytes) continue;
        array_physical = entry->base;
        break;
    }
    if (array_physical == 0)
        panic("pmm: no usable region large enough for %lu KiB of page frames", array_bytes / 1024);

    g_frames = mm::phys_to_virt<Frame>(array_physical);
    memset(g_frames, 0, array_bytes);

    // Everything starts reserved; usable regions are then handed to the buddy
    // allocator one page at a time and coalesce upwards on their own.
    for (u64 i = 0; i < memmap->entry_count; i++) {
        const auto* entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;

        u64 base = mm::align_up(entry->base, mm::kPageSize);
        const u64 end = mm::align_down(entry->base + entry->length, mm::kPageSize);

        // Physical page 0 is deliberately never allocated: a physical address
        // of 0 is this allocator's "no page" return value, and from Limine base
        // revision 3 the first page really can be reported as usable.
        if (base == 0) base = mm::kPageSize;

        // Skip the frames backing the frame array itself.
        for (u64 address = base; address < end; address += mm::kPageSize) {
            if (address >= array_physical && address < array_physical + array_bytes) continue;
            if (!valid_pfn(pfn_of(address))) continue;
            release(pfn_of(address), 0);
        }
    }

    g_stats.total_pages  = g_frame_count;
    g_stats.usable_pages = g_stats.free_pages;

    LOG_INFO("pmm: %lu MiB usable, %lu free pages, %lu frames tracked",
             usable_bytes / (1024 * 1024), g_stats.free_pages, g_frame_count);
    LOG_INFO("pmm: %lu KiB of frame records at %#lx", array_bytes / 1024, array_physical);
}

u64 alloc_pages(u8 order) {
    if (order > kMaxOrder) return 0;
    ScopedLock guard(g_lock);

    // Take from the smallest order that can satisfy the request, then split the
    // surplus back down, returning each half to its own free list.
    u8 current = order;
    while (current <= kMaxOrder && g_free_lists[current] == nullptr) current++;
    if (current > kMaxOrder) return 0;

    Frame* frame = g_free_lists[current];
    list_remove(current, frame);
    frame->flags &= static_cast<u8>(~kFrameFree);

    u64 pfn = physical_for(frame) >> mm::kPageShift;
    while (current > order) {
        current--;
        const u64 half = pfn + (1ULL << current);
        g_frames[half].flags = 0;
        mark_block_free(half, current);
    }

    Frame* head = &g_frames[pfn];
    head->order    = order;
    head->flags    = kFrameIsHead;
    head->refcount = 1;
    head->owner    = nullptr;

    g_stats.free_pages -= 1ULL << order;
    g_stats.allocations++;
    return pfn << mm::kPageShift;
}

u64 alloc_page() { return alloc_pages(0); }

u64 alloc_page_zeroed() {
    const u64 physical = alloc_page();
    if (physical != 0) memset(mm::phys_to_virt<void>(physical), 0, mm::kPageSize);
    return physical;
}

namespace {

void free_pages_locked(u64 physical, u8 order) {
    const u64 pfn = pfn_of(physical);
    if (!valid_pfn(pfn)) panic("pmm: free of out-of-range address %#lx", physical);

    Frame* frame = &g_frames[pfn];
    if (frame->flags & kFrameFree) panic("pmm: double free of %#lx", physical);
    if (frame->order != order)
        panic("pmm: free of %#lx with order %u, but it was allocated with order %u",
              physical, order, frame->order);

    frame->refcount = 0;
    frame->owner = nullptr;
    g_stats.frees++;
    release(pfn, order);
}

} // namespace

void free_pages(u64 physical, u8 order) {
    if (physical == 0) return;
    ScopedLock guard(g_lock);
    free_pages_locked(physical, order);
}

void free_page(u64 physical) { free_pages(physical, 0); }

Frame* frame_for(u64 physical) {
    const u64 pfn = pfn_of(physical);
    return valid_pfn(pfn) ? &g_frames[pfn] : nullptr;
}

u64 physical_for(const Frame* frame) {
    return static_cast<u64>(frame - g_frames) << mm::kPageShift;
}

void incref(u64 physical) {
    ScopedLock guard(g_lock);
    Frame* frame = frame_for(physical);
    if (frame == nullptr) panic("pmm: incref of out-of-range address %#lx", physical);
    frame->refcount++;
}

u16 decref(u64 physical) {
    ScopedLock guard(g_lock);
    Frame* frame = frame_for(physical);
    if (frame == nullptr) panic("pmm: decref of out-of-range address %#lx", physical);
    if (frame->refcount == 0) panic("pmm: decref of unreferenced page %#lx", physical);

    frame->refcount--;
    const u16 remaining = frame->refcount;
    if (remaining == 0) free_pages_locked(physical, frame->order);
    return remaining;
}

u16 refcount(u64 physical) {
    ScopedLock guard(g_lock);
    const Frame* frame = frame_for(physical);
    return frame ? frame->refcount : 0;
}

Stats stats() {
    ScopedLock guard(g_lock);
    return g_stats;
}

void dump_stats(const char* context) {
    const Stats snapshot = stats();
    LOG_INFO("pmm[%s]: %lu/%lu pages free (%lu MiB), %lu allocs, %lu frees, %lu outstanding",
             context, snapshot.free_pages, snapshot.usable_pages,
             (snapshot.free_pages * mm::kPageSize) / (1024 * 1024),
             snapshot.allocations, snapshot.frees, snapshot.allocations - snapshot.frees);
}

} // namespace pmm
