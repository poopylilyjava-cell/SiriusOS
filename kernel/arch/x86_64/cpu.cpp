#include <arch/x86_64/cpu.hpp>
#include <klib/panic.hpp>

namespace cpu {
namespace {

constexpr u64 kCr0MonitorCoprocessor = 1ULL << 1;
constexpr u64 kCr0Emulation          = 1ULL << 2;
constexpr u64 kCr0WriteProtect       = 1ULL << 16;
constexpr u64 kCr4Osfxsr             = 1ULL << 9;   // FXSAVE/FXRSTOR and SSE enabled
constexpr u64 kCr4Osxmmexcpt         = 1ULL << 10;  // unmasked SSE exceptions raise #XM

constexpr u64 kEferSyscall  = 1ULL << 0;
constexpr u64 kEferNoExecute = 1ULL << 11;

constexpr u32 kCpuidExtFeatures = 0x80000001;
constexpr u32 kCpuidExtNx       = 1u << 20;  // edx
constexpr u32 kCpuidExtSyscall  = 1u << 11;  // edx

alignas(16) u8 g_initial_fpu_state[kFpuStateSize];
bool g_have_initial_fpu_state = false;

} // namespace

void init_features() {
    const CpuidResult ext = cpuid(kCpuidExtFeatures);
    if (!(ext.edx & kCpuidExtNx))      panic("cpu: no NX support; the kernel's page tables require it");
    if (!(ext.edx & kCpuidExtSyscall)) panic("cpu: no SYSCALL support");

    wrmsr(msr::kEfer, rdmsr(msr::kEfer) | kEferNoExecute | kEferSyscall);

    // Without WP, ring 0 writes straight through read-only mappings: the
    // kernel's own text, and every copy-on-write page.
    u64 cr0 = read_cr0();
    cr0 &= ~kCr0Emulation;
    cr0 |= kCr0MonitorCoprocessor | kCr0WriteProtect;
    write_cr0(cr0);

    write_cr4(read_cr4() | kCr4Osfxsr | kCr4Osxmmexcpt);

    // The kernel itself never touches the FPU (-mno-sse and friends), so the
    // first CPU through here still has the reset state to capture.
    if (!g_have_initial_fpu_state) {
        // 0x1f80 is the architectural reset value: all SSE exceptions masked,
        // round to nearest. The firmware may have left anything there.
        const u32 mxcsr = 0x1f80;
        __asm__ volatile("fninit; ldmxcsr %0" : : "m"(mxcsr));
        fxsave(g_initial_fpu_state);
        g_have_initial_fpu_state = true;
    }
}

const void* initial_fpu_state() { return g_initial_fpu_state; }

} // namespace cpu
