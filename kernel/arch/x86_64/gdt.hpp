#pragma once
#include <klib/types.hpp>

struct Cpu;

// Segment selectors. The user data/code ordering is not arbitrary: SYSRET
// derives SS from STAR+8 and CS from STAR+16, so user data must sit directly
// below user code. Getting this wrong only shows up years later, in M2.
namespace gdt {

constexpr u16 kNull       = 0x00;
constexpr u16 kKernelCode = 0x08;
constexpr u16 kKernelData = 0x10;
constexpr u16 kUserData   = 0x18;
constexpr u16 kUserCode   = 0x20;
constexpr u16 kTss        = 0x28;

constexpr u16 kUserDataSelector = kUserData | 3;  // with RPL 3, as loaded in ring 3
constexpr u16 kUserCodeSelector = kUserCode | 3;

// Interrupt Stack Table slots, used by the IDT to force a known-good stack.
constexpr u8 kIstDoubleFault = 1;
constexpr u8 kIstNmi         = 2;

struct [[gnu::packed]] Tss {
    u32 reserved0;
    u64 rsp[3];       // stack pointers for ring 0..2, used on privilege change
    u64 reserved1;
    u64 ist[7];       // IST1..IST7 live at ist[0..6]
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

// Load this CPU's own GDT and TSS. Every CPU needs a TSS of its own: it holds
// that CPU's ring-0 stack and its fault stacks, and the descriptor is marked
// busy once loaded, so it cannot be shared.
void init(Cpu& cpu, u64 double_fault_stack_top, u64 nmi_stack_top);

// The bootstrap processor runs before any allocator exists, so its fault
// stacks are static.
void init_bsp(Cpu& cpu);

// The stack the CPU switches to when an interrupt or exception arrives from
// ring 3. Updated on every context switch to the incoming thread's stack.
void set_kernel_stack(Cpu& cpu, u64 stack_top);

} // namespace gdt
