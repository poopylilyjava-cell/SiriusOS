#pragma once
#include <mm/memory.hpp>

struct limine_memmap_response;

namespace pmm {

// Largest allocation is 2^kMaxOrder pages = 4 MiB. Anything bigger has no
// business being physically contiguous in this kernel.
constexpr u8 kMaxOrder = 10;

// One record per physical page frame. Kept deliberately small: there is one of
// these for every 4 KiB of RAM in the machine.
struct Frame {
    Frame* next;      // intrusive free-list links, valid only while free
    Frame* prev;
    u8     order;     // size class of the block this frame heads
    u8     flags;
    u16    refcount;  // >1 means shared, which is what makes a page copy-on-write
    u32    reserved;
    void*  owner;     // owning slab cache, or null for a raw page allocation
};

static_assert(sizeof(Frame) == 32, "Frame is per-page overhead; keep it small");

enum FrameFlags : u8 {
    kFrameFree    = 1 << 0,
    kFrameIsHead  = 1 << 1,  // heads a buddy block of size 2^order
};

struct Stats {
    u64 total_pages;
    u64 usable_pages;
    u64 free_pages;
    u64 allocations;   // lifetime counters, for the leak check
    u64 frees;
};

void init(const limine_memmap_response* memmap);

// Allocate 2^order contiguous pages. Returns a physical address, or 0.
u64  alloc_pages(u8 order);
u64  alloc_page();
u64  alloc_page_zeroed();
void free_pages(u64 physical, u8 order);
void free_page(u64 physical);

Frame* frame_for(u64 physical);
u64    physical_for(const Frame* frame);

// Reference counting, used by copy-on-write mappings.
void incref(u64 physical);
u16  decref(u64 physical);  // returns the new count; frees the page when it hits 0
u16  refcount(u64 physical);

Stats stats();
void  dump_stats(const char* context);

} // namespace pmm
