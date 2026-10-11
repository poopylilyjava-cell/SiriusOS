#include <drivers/fbconsole.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>

extern "C" { extern char __kernel_start[]; extern char __kernel_end[]; }

namespace {

struct StackFrame {
    StackFrame* next;
    u64 return_address;
};

bool plausible_frame(const StackFrame* frame) {
    const auto address = reinterpret_cast<u64>(frame);
    // Frame pointers live on a kernel stack: higher half, and 8-byte aligned.
    return address >= 0xffff800000000000ULL && (address & 0x7) == 0;
}

bool in_kernel_text(u64 address) {
    return address >= reinterpret_cast<u64>(__kernel_start) &&
           address <  reinterpret_cast<u64>(__kernel_end);
}

// The panic path can itself fault -- a corrupt frame pointer sends the
// backtrace walk into unmapped memory -- and that fault panics again, walking
// the same chain. One level of nesting is reported; after that, just stop.
bool g_panicking = false;

void enter_panic() {
    cpu::cli();
    if (g_panicking) {
        kprintf("\n*** panic while panicking -- halted ***\n");
        cpu::halt_forever();
    }
    g_panicking = true;
    // No serial port to read this from, on most machines: put it on screen.
    fbconsole::take_over();
}

} // namespace

void print_backtrace(u64 rbp) {
    kprintf("backtrace:\n");

    auto* frame = reinterpret_cast<StackFrame*>(rbp);
    for (int depth = 0; depth < 24; depth++) {
        if (!plausible_frame(frame)) break;
        const u64 ret = frame->return_address;
        if (ret == 0) break;

        kprintf("  #%-2d %p%s\n", depth, reinterpret_cast<void*>(ret),
                in_kernel_text(ret) ? "" : "  <outside kernel text>");

        if (!plausible_frame(frame->next) || frame->next <= frame) break;
        frame = frame->next;
    }
    kprintf("  (resolve with: tools/symbolize.sh)\n");
}

[[noreturn]] void panic(const char* fmt, ...) {
    enter_panic();

    kprintf("\n*** KERNEL PANIC ***\n");

    char line[512];
    va_list args;
    va_start(args, fmt);
    kvsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    kprintf("%s\n", line);

    u64 rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    print_backtrace(rbp);

    kprintf("*** halted ***\n");
    cpu::halt_forever();
}

[[noreturn]] void panic_with_registers(const Registers* r, const char* reason) {
    enter_panic();

    kprintf("\n*** KERNEL PANIC ***\n");
    kprintf("%s: vector %lu (%s), error code %#lx\n",
            reason, r->vector, idt::exception_name(r->vector), r->error_code);

    // A page fault's faulting address is only available in CR2, and only until
    // the next one -- read it here or lose it.
    if (r->vector == 14) {
        const u64 cr2 = cpu::read_cr2();
        kprintf("faulting address: %p  [%s %s %s%s]\n",
                reinterpret_cast<void*>(cr2),
                (r->error_code & 0x1) ? "protection-violation" : "not-present",
                (r->error_code & 0x2) ? "write" : "read",
                (r->error_code & 0x4) ? "user" : "kernel",
                (r->error_code & 0x10) ? " instruction-fetch" : "");
    }

    kprintf("rip=%p  cs=%#lx  rflags=%#lx\n",
            reinterpret_cast<void*>(r->rip), r->cs, r->rflags);
    kprintf("rsp=%p  ss=%#lx\n", reinterpret_cast<void*>(r->rsp), r->ss);
    kprintf("rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("rsi=%016lx rdi=%016lx rbp=%016lx\n", r->rsi, r->rdi, r->rbp);
    kprintf("r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx\n", r->r8, r->r9, r->r10, r->r11);
    kprintf("r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n", r->r12, r->r13, r->r14, r->r15);
    kprintf("cr0=%016lx cr2=%016lx cr3=%016lx cr4=%016lx\n",
            cpu::read_cr0(), cpu::read_cr2(), cpu::read_cr3(), cpu::read_cr4());

    print_backtrace(r->rbp);

    kprintf("*** halted ***\n");
    cpu::halt_forever();
}
