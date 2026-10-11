# ADR 0005 — Buddy allocator, direct-mapped physical memory, shared kernel half

**Status:** accepted · 2026-09-16 · **milestone:** M1

## Context
The kernel needs a physical page allocator, a virtual memory layer, and a heap.
Every later subsystem depends on the shape of these three.

## Decisions

### Buddy allocator over a page-frame array
One `Frame` record per physical page (32 bytes), with per-order free lists and
power-of-two coalescing. The `Frame` carries a reference count, which is what
makes copy-on-write possible without a second data structure.

*Rejected:* a plain bitmap. It cannot allocate contiguous runs efficiently and
has nowhere to record a reference count. The bitmap remains the right idea only
for bootstrapping, and the frame array subsumes it.

### Physical page 0 is never allocated
A physical address of 0 is this allocator's "no page" return value, and from
Limine base revision 3 the first page really can be reported as usable. Handing
it out makes a successful allocation indistinguishable from failure.

### Physical memory reached through the direct map
The kernel rebuilds the bootloader's higher-half direct map in its own page
tables, using 2 MiB pages. Mapping ~2 GiB with 4 KiB entries would cost several
MiB of page tables by itself; the large-page mapping costs 271.

Framebuffer regions are mapped write-combining (PAT5, which Limine guarantees).
Reserved and bad regions are deliberately left unmapped.

### The kernel half is pre-allocated and shared by pointer
All 256 higher-half PML4 entries are allocated at boot. A new address space
copies those 256 entries once; because the entries themselves never change, a
kernel mapping made later is automatically visible in every address space with
no synchronisation. The cost is 256 pages, paid once.

The consequence is that `unmap` must never free a kernel-half PML4 entry: other
address spaces hold copies of it.

### Software bits carry the paging policy
Bits 9-11 of a page table entry are ignored by the hardware. Bit 9 marks a
demand-paged reservation, bit 10 a copy-on-write page. A non-present entry's
other bits are also ignored by the CPU, so the intended flags are parked in the
entry itself until the fault arrives -- no side table.

### Slab heap with metadata in the slab
Power-of-two size classes from 16 to 2048 bytes; larger requests go straight to
the buddy allocator. The slab header lives at the front of the slab, and since
a buddy block of order N is 2^N-page aligned, any pointer can be masked back to
its header. Each page's `Frame.owner` names the owning cache, so `kfree` needs
no per-object header at all.

## Consequences
- `unmap` prunes page tables that become empty. Without it, map/unmap churn
  leaks tables that nothing ever reclaims.
- Page faults are resolved against whichever address space is in CR3, not
  always the kernel's. M2 makes that the current process's space.
- The bootloader-reclaimable memory is *not* reclaimed: the Limine responses
  still live there. Reclaiming it is M2 work.
- The kernel leaves the bootloader's stack as soon as its page tables are
  live. That stack is addressed through the direct map, which is contiguous:
  an overflow there does not fault, it overwrites the physical memory below.
  Kernel stacks are instead mapped in their own range (PML4 slot 384) with an
  unmapped guard page on either side, so an overflow becomes a #DF on IST1.
- Only pages that were writable are marked copy-on-write when shared. The
  fault handler restores write access to anything carrying the marker, so
  marking a read-only page would make it writable on the first write.
