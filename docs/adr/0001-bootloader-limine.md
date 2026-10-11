# ADR 0001 — Limine as the bootloader

**Status:** accepted · 2026-09-16

## Context
The kernel needs to reach 64-bit long mode in the higher half with a framebuffer,
a memory map, and secondary CPUs available, on both UEFI and legacy BIOS.

## Decision
Use Limine (v11.x binary branch) and the Limine boot protocol.

## Alternatives rejected
- **GRUB + multiboot2** — does not set up long mode, the higher half, or SMP. That
  is several hundred lines of assembly and a week of debugging for no benefit.
- **Own UEFI bootloader** — educational, but it is a second project, and the
  interesting work here is above the bootloader, not inside it.

## Consequences
- The protocol header comes from a second submodule, `limine-protocol`; since
  v11 the binary branch no longer ships `limine.h`.
- Boot requests must be kept alive through `--gc-sections`; the linker script
  `KEEP`s `.limine_requests*`.
- Limine gives us a higher-half direct map (HHDM) from the start, so early
  physical memory access does not need bootstrap page tables of our own.
