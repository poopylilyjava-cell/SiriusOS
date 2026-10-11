#pragma once
#include <klib/types.hpp>

namespace cpu {

inline void outb(u16 port, u8 value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline u8 inb(u16 port) {
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outw(u16 port, u16 value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline u16 inw(u16 port) {
    u16 value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outl(u16 port, u32 value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline u32 inl(u16 port) {
    u32 value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void io_wait() { outb(0x80, 0); }

inline void cli() { __asm__ volatile("cli"); }
inline void sti() { __asm__ volatile("sti"); }

[[noreturn]] inline void halt_forever() {
    for (;;) __asm__ volatile("cli; hlt");
}

inline u64 read_cr0() { u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
inline u64 read_cr2() { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
inline u64 read_cr3() { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
inline u64 read_cr4() { u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
inline void write_cr0(u64 v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
inline void write_cr4(u64 v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

// Model-specific registers.
namespace msr {
constexpr u32 kApicBase     = 0x0000001b;
constexpr u32 kEfer         = 0xc0000080;
constexpr u32 kStar         = 0xc0000081;  // SYSCALL/SYSRET segment selectors
constexpr u32 kLstar        = 0xc0000082;  // SYSCALL entry point
constexpr u32 kSfmask       = 0xc0000084;  // RFLAGS bits cleared on SYSCALL
constexpr u32 kFsBase       = 0xc0000100;  // user TLS: the thread control block
constexpr u32 kGsBase       = 0xc0000101;
constexpr u32 kKernelGsBase = 0xc0000102;  // swapped with GS base by SWAPGS
} // namespace msr

inline u64 rdmsr(u32 index) {
    u32 low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return (static_cast<u64>(high) << 32) | low;
}

inline void wrmsr(u32 index, u64 value) {
    __asm__ volatile("wrmsr" : : "c"(index), "a"(static_cast<u32>(value)),
                     "d"(static_cast<u32>(value >> 32)) : "memory");
}

struct CpuidResult { u32 eax, ebx, ecx, edx; };

inline CpuidResult cpuid(u32 leaf, u32 subleaf = 0) {
    CpuidResult r;
    __asm__ volatile("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(subleaf));
    return r;
}

inline u64 read_tsc() {
    u32 low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<u64>(high) << 32) | low;
}

// FXSAVE/FXRSTOR images are 512 bytes and must be 16-byte aligned.
constexpr usize kFpuStateSize = 512;
inline void fxsave(void* area)        { __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory"); }
inline void fxrstor(const void* area) { __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory"); }

// Switch to a new stack and call `entry`, which must not return. The frame
// pointer is cleared so backtraces end at the bottom of the new stack.
[[noreturn]] inline void enter_on_stack(u64 stack_top, void (*entry)()) {
    __asm__ volatile(
        "mov %0, %%rsp\n"
        "xor %%ebp, %%ebp\n"
        "call *%1\n"
        "ud2\n"
        : : "r"(stack_top), "r"(entry) : "memory");
    __builtin_unreachable();
}

// Put this CPU's control registers into the state the kernel assumes: NX
// usable in page tables, write protection honoured in ring 0, SYSCALL enabled,
// and SSE available to user code. Must run on every CPU before it loads the
// kernel's page tables, which use the NX bit. Panics if the CPU cannot.
void init_features();

// The FXSAVE image of a freshly initialised FPU, copied into every new user
// thread. Captured by init_features() on the bootstrap processor.
const void* initial_fpu_state();

} // namespace cpu
