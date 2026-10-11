#pragma once
#include <klib/types.hpp>

// Saved machine state at the point of an interrupt or exception.
//
// Field order mirrors the stack layout built by isr_common in isr.S: the last
// register pushed sits at the lowest address and so comes first here. Changing
// either side without the other silently corrupts every register dump.
struct [[gnu::packed]] Registers {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    u64 vector;      // pushed by the per-vector stub
    u64 error_code;  // pushed by the CPU, or a zero placeholder by the stub

    // Pushed by the CPU. In long mode ss:rsp are pushed even without a
    // privilege change, so this frame is the same shape for kernel faults.
    u64 rip, cs, rflags, rsp, ss;
};

static_assert(sizeof(Registers) == 22 * 8, "Registers must match the isr_common stack frame");

// Vectors 0-31 are CPU exceptions. External interrupts are placed above them.
constexpr u64 kVectorTimer      = 0x20;  // local APIC timer: the scheduler tick
constexpr u64 kVectorReschedule = 0x21;  // IPI: "you have new work"
constexpr u64 kVectorKeyboard   = 0x30;  // device interrupts, routed by the I/O APIC
constexpr u64 kVectorMouse      = 0x31;
constexpr u64 kVectorPicBase   = 0xe0;  // legacy PIC, remapped here and masked
constexpr u64 kVectorSpurious  = 0xff;  // local APIC spurious interrupt

inline bool from_user_mode(const Registers* regs) { return (regs->cs & 3) != 0; }

namespace idt {

// Build the table once, on the bootstrap processor, and load it.
void init();
// Load the already-built table on an application processor.
void load();

// Human-readable name for CPU exception vectors 0..31.
const char* exception_name(u64 vector);

} // namespace idt

namespace interrupts {

// Call `handler` whenever `vector` arrives, on whichever CPU takes it. It
// runs with interrupts off, on the interrupted thread's kernel stack, and
// must not block. The interrupt is acknowledged after it returns. Handlers
// are registered at boot, before their device can interrupt.
using Handler = void (*)();
void set_handler(u64 vector, Handler handler);

} // namespace interrupts

extern "C" void isr_dispatch(Registers* regs);
