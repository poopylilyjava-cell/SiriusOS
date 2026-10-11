#include <arch/x86_64/smp.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/lapic.hpp>
#include <arch/x86_64/refclock.hpp>
#include <arch/x86_64/percpu.hpp>
#include <arch/x86_64/syscall.hpp>
#include <boot/requests.hpp>
#include <sched/sched.hpp>
#include <mm/vmm.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>

namespace smp {
namespace {

constexpr u64 kBootStackPages  = 16;  // becomes the idle thread's stack
constexpr u64 kFaultStackPages = 4;   // #DF and NMI, each with guard pages
constexpr u64 kStartTimeoutMs  = 1000;

// What the BSP prepares for each AP before releasing it. Read by the AP
// through Cpu::index, since the Limine stack it arrives on is not ours.
struct ApStart {
    u64 stack_top;
    u64 double_fault_stack_top;
    u64 nmi_stack_top;
};

ApStart g_ap_start[percpu::kMaxCpus];
u32 g_online = 0;

// Everything after the stack switch. The AP's own record is in GS by now.
[[noreturn]] void ap_main() {
    Cpu* cpu = percpu::current();

    lapic::init_cpu();
    sched::init_ap(g_ap_start[cpu->index].stack_top);

    LOG_DEBUG("smp: cpu %u (lapic %u) online", cpu->index, cpu->lapic_id);
    __atomic_fetch_add(&g_online, 1, __ATOMIC_RELEASE);
    sched::run_idle();
}

// Where Limine sends each AP. It arrives in long mode on the bootloader's page
// tables, GDT and a small stack of Limine's, with interrupts off.
[[noreturn]] void ap_entry(limine_mp_info* info) {
    auto* cpu = reinterpret_cast<Cpu*>(info->extra_argument);

    // NX first: the kernel's page tables use it, and loading them on a CPU
    // without EFER.NXE turns every NX entry into a reserved-bit fault.
    cpu::init_features();
    percpu::install(cpu);
    vmm::init_ap();

    const ApStart& start = g_ap_start[cpu->index];
    gdt::init(*cpu, start.double_fault_stack_top, start.nmi_stack_top);
    idt::load();
    syscall::init_cpu();

    cpu::enter_on_stack(start.stack_top, ap_main);
}

} // namespace

u32 start_aps() {
    const limine_mp_response* response = g_mp_request.response;
    if (response == nullptr) {
        LOG_WARN("smp: bootloader started no application processors");
        return 0;
    }

    u32 released = 0;
    for (u64 i = 0; i < response->cpu_count; i++) {
        limine_mp_info* info = response->cpus[i];
        if (info->lapic_id == response->bsp_lapic_id) continue;

        Cpu* cpu = percpu::create(info->lapic_id);
        if (cpu == nullptr) {
            LOG_WARN("smp: cpu with lapic %u ignored: limit is %u cpus", info->lapic_id, percpu::kMaxCpus);
            continue;
        }

        ApStart& start = g_ap_start[cpu->index];
        start.stack_top              = vmm::create_kernel_stack(kBootStackPages);
        start.double_fault_stack_top = vmm::create_kernel_stack(kFaultStackPages);
        start.nmi_stack_top          = vmm::create_kernel_stack(kFaultStackPages);
        if (!start.stack_top || !start.double_fault_stack_top || !start.nmi_stack_top)
            panic("smp: out of memory for cpu %u's stacks", cpu->index);

        info->extra_argument = reinterpret_cast<u64>(cpu);
        // The AP spins on this field; writing it is what starts the CPU.
        __atomic_store_n(&info->goto_address, &ap_entry, __ATOMIC_RELEASE);
        released++;
    }

    // Wait for them, bounded: a CPU that never arrives is reported, not
    // waited on forever.
    for (u64 waited = 0; waited < kStartTimeoutMs; waited++) {
        if (__atomic_load_n(&g_online, __ATOMIC_ACQUIRE) == released) break;
        refclock::delay_us(1000);
    }

    const u32 online = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
    if (online != released) LOG_ERROR("smp: only %u of %u application processors came up", online, released);
    LOG_INFO("smp: %u cpus online", online + 1);
    return online;
}

} // namespace smp
