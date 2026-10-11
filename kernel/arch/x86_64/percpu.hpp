#pragma once
#include <klib/types.hpp>

namespace sched { struct Thread; struct RunQueue; }
namespace vmm { class AddressSpace; }
namespace gdt { struct Tss; }

// Everything that exists once per CPU. In kernel mode the GS base points at
// the running CPU's record, so `%gs:0` (the self pointer) is the cheapest way
// to find it; in user mode SWAPGS has parked that pointer in KERNEL_GS_BASE.
//
// The first three fields are read by assembly (syscall.S) at fixed offsets.
struct Cpu {
    Cpu* self;                    // %gs:0
    u64  syscall_stack_top;       // %gs:8   kernel stack of the running thread
    u64  user_rsp_scratch;        // %gs:16  user RSP, briefly, on SYSCALL entry

    u32  index;                   // dense, 0 = bootstrap processor
    u32  lapic_id;
    bool online;

    gdt::Tss*          tss;
    vmm::AddressSpace* address_space;  // whatever is loaded in CR3

    sched::Thread*   current;
    sched::Thread*   idle;
    sched::Thread*   switch_prev;      // the thread a context switch just left
    sched::RunQueue* runqueue;
    bool need_resched;            // a thread was woken here: give it the CPU at the next safe point

    u64 ticks;                    // timer interrupts taken on this CPU
    u64 idle_ticks;               // ...of which found it running its idle thread
};

namespace percpu {

constexpr u32 kMaxCpus = 64;

// The bootstrap processor's record is static, so it exists before the heap.
// Installs it in GS; call before anything that might use current().
void init_bsp();

// Allocate a record for an application processor. Returns null when full.
Cpu* create(u32 lapic_id);

// Point this CPU's GS base at `cpu`.
void install(Cpu* cpu);

u32  count();
Cpu* at(u32 index);

inline Cpu* current() {
    Cpu* cpu;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

} // namespace percpu
