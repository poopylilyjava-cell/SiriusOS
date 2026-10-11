#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/percpu.hpp>
#include <klib/string.hpp>
#include <klib/print.hpp>

namespace gdt {
namespace {

struct [[gnu::packed]] DescriptorPointer {
    u16 limit;
    u64 base;
};

struct [[gnu::packed]] TssDescriptor {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
    u32 base_upper;
    u32 reserved;
};

struct [[gnu::packed]] GlobalDescriptorTable {
    u64 null;
    u64 kernel_code;
    u64 kernel_data;
    u64 user_data;
    u64 user_code;
    TssDescriptor tss;
};

// Long-mode code/data descriptors. In 64-bit mode base and limit are ignored
// for these, so only the access and flag bits carry meaning.
//   0x9a = present, ring 0, code, readable      0x92 = present, ring 0, data, writable
//   0xfa = present, ring 3, code, readable      0xf2 = present, ring 3, data, writable
//   0xa0 in the flags nibble = long mode (L), granularity irrelevant
constexpr u64 kKernelCodeDescriptor = 0x00af9a000000ffffULL;
constexpr u64 kKernelDataDescriptor = 0x00af92000000ffffULL;
constexpr u64 kUserDataDescriptor   = 0x00aff2000000ffffULL;
constexpr u64 kUserCodeDescriptor   = 0x00affa000000ffffULL;

// One GDT and TSS per possible CPU, indexed by Cpu::index. Static, because the
// bootstrap processor needs its pair before the heap exists.
alignas(16) GlobalDescriptorTable g_gdts[percpu::kMaxCpus];
alignas(16) Tss g_tsses[percpu::kMaxCpus];

// A double fault means the normal stack may be unusable -- typically because it
// overflowed. Handling it on a dedicated IST stack is what turns an instant
// triple-fault reboot into a readable register dump.
constexpr usize kFaultStackSize = 16 * 1024;
alignas(16) u8 g_bsp_double_fault_stack[kFaultStackSize];
alignas(16) u8 g_bsp_nmi_stack[kFaultStackSize];

} // namespace

void init(Cpu& cpu, u64 double_fault_stack_top, u64 nmi_stack_top) {
    GlobalDescriptorTable& gdt = g_gdts[cpu.index];
    Tss& tss = g_tsses[cpu.index];

    memset(&tss, 0, sizeof(tss));
    tss.ist[kIstDoubleFault - 1] = double_fault_stack_top;
    tss.ist[kIstNmi - 1]         = nmi_stack_top;
    // No I/O permission bitmap: point past the segment limit to disable it.
    tss.iomap_base = sizeof(Tss);
    cpu.tss = &tss;

    gdt.null        = 0;
    gdt.kernel_code = kKernelCodeDescriptor;
    gdt.kernel_data = kKernelDataDescriptor;
    gdt.user_data   = kUserDataDescriptor;
    gdt.user_code   = kUserCodeDescriptor;

    const auto tss_base = reinterpret_cast<u64>(&tss);
    gdt.tss = {
        .limit_low   = static_cast<u16>(sizeof(Tss) - 1),
        .base_low    = static_cast<u16>(tss_base & 0xffff),
        .base_mid    = static_cast<u8>((tss_base >> 16) & 0xff),
        .access      = 0x89, // present, type 9 = available 64-bit TSS
        .granularity = 0x00,
        .base_high   = static_cast<u8>((tss_base >> 24) & 0xff),
        .base_upper  = static_cast<u32>(tss_base >> 32),
        .reserved    = 0,
    };

    const DescriptorPointer pointer{
        .limit = static_cast<u16>(sizeof(gdt) - 1),
        .base  = reinterpret_cast<u64>(&gdt),
    };

    // Loading the GDT does not reload the segment registers. CS can only be
    // changed by a far transfer, so we push a target selector and lretq to it.
    //
    // FS and GS get the null selector: in long mode their bases come from
    // MSRs, not the descriptor. Loading any selector into GS can reset its
    // base, so the per-CPU pointer is installed again afterwards.
    __asm__ volatile(
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov %w2, %%ds\n"
        "mov %w2, %%es\n"
        "mov %w2, %%ss\n"
        "mov %w3, %%fs\n"
        "mov %w3, %%gs\n"
        :
        : "m"(pointer), "i"(static_cast<u64>(kKernelCode)), "r"(kKernelData), "r"(kNull)
        : "rax", "memory");
    percpu::install(&cpu);

    __asm__ volatile("ltr %w0" : : "r"(kTss) : "memory");

    LOG_DEBUG("gdt: cpu %u loaded, tss at %p (ist1 df=%p ist2 nmi=%p)",
              cpu.index, &tss,
              reinterpret_cast<void*>(tss.ist[kIstDoubleFault - 1]),
              reinterpret_cast<void*>(tss.ist[kIstNmi - 1]));
}

void init_bsp(Cpu& cpu) {
    init(cpu,
         reinterpret_cast<u64>(g_bsp_double_fault_stack + kFaultStackSize),
         reinterpret_cast<u64>(g_bsp_nmi_stack + kFaultStackSize));
    LOG_INFO("gdt: loaded, tss at %p", cpu.tss);
}

void set_kernel_stack(Cpu& cpu, u64 stack_top) { cpu.tss->rsp[0] = stack_top; }

} // namespace gdt
