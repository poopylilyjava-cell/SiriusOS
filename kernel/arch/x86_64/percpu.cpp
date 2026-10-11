#include <arch/x86_64/percpu.hpp>
#include <arch/x86_64/cpu.hpp>
#include <mm/heap.hpp>
#include <stddef.h>

static_assert(offsetof(Cpu, self) == 0,               "syscall.S reads %gs:0");
static_assert(offsetof(Cpu, syscall_stack_top) == 8,  "syscall.S reads %gs:8");
static_assert(offsetof(Cpu, user_rsp_scratch) == 16,  "syscall.S writes %gs:16");

namespace percpu {
namespace {

Cpu  g_bsp{};
Cpu* g_cpus[kMaxCpus] = {};
u32  g_count = 0;

} // namespace

void install(Cpu* cpu) {
    cpu::wrmsr(cpu::msr::kGsBase, reinterpret_cast<u64>(cpu));
    // User GS starts out zero; SWAPGS on the way to ring 3 makes it current.
    cpu::wrmsr(cpu::msr::kKernelGsBase, 0);
}

void init_bsp() {
    g_bsp.self  = &g_bsp;
    g_bsp.index = 0;
    g_cpus[0]   = &g_bsp;
    g_count     = 1;
    install(&g_bsp);
}

Cpu* create(u32 lapic_id) {
    // Only the BSP calls this, before any AP is started, so no lock.
    if (g_count >= kMaxCpus) return nullptr;
    auto* cpu = static_cast<Cpu*>(kzalloc(sizeof(Cpu)));
    if (cpu == nullptr) return nullptr;
    cpu->self     = cpu;
    cpu->index    = g_count;
    cpu->lapic_id = lapic_id;
    g_cpus[g_count++] = cpu;
    return cpu;
}

u32  count()          { return __atomic_load_n(&g_count, __ATOMIC_ACQUIRE); }
Cpu* at(u32 index)    { return index < count() ? g_cpus[index] : nullptr; }

} // namespace percpu
