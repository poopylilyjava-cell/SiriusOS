#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/cpu.hpp>
#include <klib/print.hpp>
#include <klib/panic.hpp>
#include <klib/string.hpp>
#include <arch/x86_64/lapic.hpp>
#include <mm/vmm.hpp>
#include <sched/sched.hpp>
#include <sched/process.hpp>
#include <sched/events.hpp>
#include <sched/signal.hpp>
#include <arch/x86_64/percpu.hpp>

// Built by isr.S: 256 entry points, one per vector.
extern "C" { extern void* isr_stub_table[]; }

namespace {

struct [[gnu::packed]] IdtEntry {
    u16 offset_low;
    u16 selector;
    u8  ist;        // low 3 bits select an IST slot; 0 means "use the current stack"
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
};

struct [[gnu::packed]] IdtPointer {
    u16 limit;
    u64 base;
};

static_assert(sizeof(IdtEntry) == 16, "IDT gate descriptors are 16 bytes in long mode");

alignas(16) IdtEntry g_idt[256];

interrupts::Handler g_handlers[256];

// 0x8e = present, ring 0, 64-bit interrupt gate (clears IF on entry).
constexpr u8 kInterruptGate = 0x8e;

void set_gate(u8 vector, void* handler, u8 ist = 0) {
    const auto address = reinterpret_cast<u64>(handler);
    g_idt[vector] = {
        .offset_low  = static_cast<u16>(address & 0xffff),
        .selector    = gdt::kKernelCode,
        .ist         = static_cast<u8>(ist & 0x7),
        .type_attr   = kInterruptGate,
        .offset_mid  = static_cast<u16>((address >> 16) & 0xffff),
        .offset_high = static_cast<u32>(address >> 32),
        .reserved    = 0,
    };
}

const char* const kExceptionNames[32] = {
    "#DE divide error",            "#DB debug",
    "NMI",                         "#BP breakpoint",
    "#OF overflow",                "#BR bound range exceeded",
    "#UD invalid opcode",          "#NM device not available",
    "#DF double fault",            "coprocessor segment overrun",
    "#TS invalid TSS",             "#NP segment not present",
    "#SS stack-segment fault",     "#GP general protection fault",
    "#PF page fault",              "reserved (15)",
    "#MF x87 floating-point",      "#AC alignment check",
    "#MC machine check",           "#XM SIMD floating-point",
    "#VE virtualisation",          "#CP control protection",
    "reserved (22)",               "reserved (23)",
    "reserved (24)",               "reserved (25)",
    "reserved (26)",               "reserved (27)",
    "#HV hypervisor injection",    "#VC VMM communication",
    "#SX security exception",      "reserved (31)",
};

} // namespace

void interrupts::set_handler(u64 vector, Handler handler) {
    if (vector >= 32 && vector < 256) __atomic_store_n(&g_handlers[vector], handler, __ATOMIC_RELEASE);
}

const char* idt::exception_name(u64 vector) {
    if (vector < 32) return kExceptionNames[vector];
    return "external interrupt";
}

void idt::load() {
    const IdtPointer pointer{
        .limit = static_cast<u16>(sizeof(g_idt) - 1),
        .base  = reinterpret_cast<u64>(g_idt),
    };
    __asm__ volatile("lidt %0" : : "m"(pointer) : "memory");
}

void idt::init() {
    memset(g_idt, 0, sizeof(g_idt));

    for (int vector = 0; vector < 256; vector++)
        set_gate(static_cast<u8>(vector), isr_stub_table[vector]);

    // These two must run on a stack we know is good: a double fault usually
    // means the kernel stack itself is unusable, and an NMI can land anywhere.
    set_gate(8, isr_stub_table[8], gdt::kIstDoubleFault);
    set_gate(2, isr_stub_table[2], gdt::kIstNmi);

    load();

    LOG_INFO("idt: 256 gates loaded (ist%u for #DF, ist%u for NMI)",
             gdt::kIstDoubleFault, gdt::kIstNmi);
}

namespace {

void dispatch(Registers* regs) {
    const u64 vector = regs->vector;

    // A page fault is the one exception that is routinely recoverable: demand
    // paging and copy-on-write both work by faulting on purpose.
    if (vector == 14 && vmm::handle_page_fault(regs)) return;

    if (vector < 32) {
        // A user program's fault is that program's problem, not the kernel's.
        if (from_user_mode(regs)) { sched::fault_in_user_mode(regs); return; }
        if (sched::absorb_kernel_fault(regs)) return;
        panic_with_registers(regs, "unhandled CPU exception");
    }

    if (vector == kVectorTimer) {
        // Acknowledge first: tick() may switch threads, and the next tick must
        // still be deliverable to whatever runs after this one.
        lapic::eoi();
        // One CPU keeps the system's time: timers that expire, sleepers
        // whose deadline has come.
        if (percpu::current()->index == 0) { signal::tick(); events::tick(); }
        sched::tick();
        return;
    }

    if (vector == kVectorReschedule) {
        lapic::eoi();
        sched::reschedule_interrupt();
        return;
    }

    if (const interrupts::Handler handler = __atomic_load_n(&g_handlers[vector], __ATOMIC_ACQUIRE)) {
        handler();
        lapic::eoi();
        return;
    }

    // Spurious interrupts are not real requests and must not be acknowledged.
    // The legacy PICs are masked, but can still raise a spurious IRQ 7 or 15.
    if (vector == kVectorSpurious) return;
    if (vector >= kVectorPicBase && vector < kVectorPicBase + 16) return;

    LOG_WARN("unexpected interrupt vector %lu at rip=%p", vector,
             reinterpret_cast<void*>(regs->rip));
    lapic::eoi();
}

} // namespace

extern "C" void isr_dispatch(Registers* regs) {
    dispatch(regs);
    // A handler may have woken a thread -- the disk finishing what one was
    // waiting for. If what was interrupted had interrupts on, it holds no
    // spinlock and can be switched away from here, as the timer tick does.
    if (regs->rflags & (1 << 9)) sched::preempt_point();
    // On the way back to a user program is where its signals are acted on:
    // one posted from elsewhere is noticed at the next tick at the latest.
    if (from_user_mode(regs)) signal::on_interrupt_return(regs);
}
