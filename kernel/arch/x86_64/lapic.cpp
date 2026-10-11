#include <arch/x86_64/lapic.hpp>
#include <arch/x86_64/refclock.hpp>
#include <arch/x86_64/clock.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <mm/vmm.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <klib/spinlock.hpp>
#include <arch/x86_64/percpu.hpp>

namespace lapic {
namespace {

// Register offsets in the xAPIC MMIO page. The x2APIC exposes the same
// registers as MSRs at 0x800 + offset / 16.
constexpr u32 kRegId           = 0x020;
constexpr u32 kRegVersion      = 0x030;
constexpr u32 kRegTaskPriority = 0x080;
constexpr u32 kRegEoi          = 0x0b0;
constexpr u32 kRegSpurious     = 0x0f0;
constexpr u32 kRegErrorStatus  = 0x280;
constexpr u32 kRegLvtTimer     = 0x320;
constexpr u32 kRegLvtLint0     = 0x350;
constexpr u32 kRegLvtLint1     = 0x360;
constexpr u32 kRegLvtError     = 0x370;
constexpr u32 kRegTimerInitial = 0x380;
constexpr u32 kRegTimerCurrent = 0x390;
constexpr u32 kRegTimerDivide  = 0x3e0;
constexpr u32 kRegIcrLow       = 0x300;
constexpr u32 kRegIcrHigh      = 0x310;
constexpr u32 kIcrDeliveryPending = 1u << 12;

constexpr u32 kSpuriousEnable  = 1u << 8;
constexpr u32 kLvtMasked       = 1u << 16;
constexpr u32 kLvtTimerPeriodic = 1u << 17;
constexpr u32 kTimerDivideBy16 = 0x3;

constexpr u64 kApicBaseX2apic  = 1ULL << 10;
constexpr u64 kApicBaseEnable  = 1ULL << 11;
constexpr u64 kApicBaseAddress = 0xfffff000ULL;

bool g_x2apic = false;
volatile u32* g_mmio = nullptr;
u32 g_ticks_per_period = 0;  // timer counts per tick, at divide-by-16

u32 read(u32 reg) {
    if (g_x2apic) return static_cast<u32>(cpu::rdmsr(0x800 + (reg >> 4)));
    return g_mmio[reg / 4];
}

void write(u32 reg, u32 value) {
    if (g_x2apic) { cpu::wrmsr(0x800 + (reg >> 4), value); return; }
    g_mmio[reg / 4] = value;
}

// --- legacy 8259 PICs -------------------------------------------------------
// Limine hands over with them masked, but masked PICs still raise spurious
// IRQs, and at their power-on vectors those would land on CPU exceptions 8-15.
// Moving them above the exceptions makes a stray one harmless.
void retire_legacy_pics() {
    constexpr u16 kMasterCommand = 0x20, kMasterData = 0x21;
    constexpr u16 kSlaveCommand  = 0xa0, kSlaveData  = 0xa1;

    cpu::outb(kMasterCommand, 0x11); cpu::io_wait();  // ICW1: init, expect ICW4
    cpu::outb(kSlaveCommand,  0x11); cpu::io_wait();
    cpu::outb(kMasterData, static_cast<u8>(kVectorPicBase));     cpu::io_wait();
    cpu::outb(kSlaveData,  static_cast<u8>(kVectorPicBase + 8)); cpu::io_wait();
    cpu::outb(kMasterData, 0x04); cpu::io_wait();     // slave on IRQ 2
    cpu::outb(kSlaveData,  0x02); cpu::io_wait();
    cpu::outb(kMasterData, 0x01); cpu::io_wait();     // 8086 mode
    cpu::outb(kSlaveData,  0x01); cpu::io_wait();
    cpu::outb(kMasterData, 0xff);                     // mask everything
    cpu::outb(kSlaveData,  0xff);
}

void calibrate_timer() {
    constexpr u64 kCalibrationMs = 50;
    const u64 period_us = 1'000'000 / kTimerHz;

    write(kRegTimerDivide, kTimerDivideBy16);
    write(kRegLvtTimer, kLvtMasked | static_cast<u32>(kVectorTimer));

    // The same 50 ms measure the time stamp counter, for the clock.
    write(kRegTimerInitial, 0xffffffff);
    const u64 tsc_start = cpu::read_tsc();
    refclock::delay_us(kCalibrationMs * 1000);
    const u64 tsc_elapsed = cpu::read_tsc() - tsc_start;
    const u32 elapsed = 0xffffffff - read(kRegTimerCurrent);
    write(kRegTimerInitial, 0);
    clock::init(tsc_elapsed * 1000 / kCalibrationMs);

    if (elapsed == 0) panic("lapic: timer did not count during calibration");
    g_ticks_per_period = static_cast<u32>(static_cast<u64>(elapsed) * period_us / (kCalibrationMs * 1000));

    LOG_INFO("lapic: timer %lu kHz (div 16), %u counts per %u Hz tick",
             static_cast<u64>(elapsed) / kCalibrationMs, g_ticks_per_period, kTimerHz);
}

} // namespace

void init_bsp() {
    const u64 base = cpu::rdmsr(cpu::msr::kApicBase);
    if (!(base & kApicBaseEnable)) panic("lapic: the local APIC is disabled");
    g_x2apic = (base & kApicBaseX2apic) != 0;

    if (!g_x2apic) {
        const u64 address = vmm::map_mmio(base & kApicBaseAddress, mm::kPageSize);
        if (address == 0) panic("lapic: cannot map the APIC registers");
        g_mmio = reinterpret_cast<volatile u32*>(address);
    }

    percpu::current()->lapic_id = id();
    retire_legacy_pics();
    LOG_INFO("lapic: %s mode, version %#x, bsp id %u",
             g_x2apic ? "x2apic" : "xapic", read(kRegVersion) & 0xff, id());

    calibrate_timer();
    init_cpu();
}

void init_cpu() {
    // Every CPU shares one mode: Limine leaves the APs as it found the BSP.
    if (g_x2apic && !(cpu::rdmsr(cpu::msr::kApicBase) & kApicBaseX2apic))
        panic("lapic: cpu is not in x2apic mode like the bsp");

    write(kRegTaskPriority, 0);  // accept every interrupt class
    write(kRegLvtLint0, kLvtMasked);
    write(kRegLvtLint1, kLvtMasked);
    write(kRegLvtError, kLvtMasked);
    write(kRegSpurious, kSpuriousEnable | static_cast<u32>(kVectorSpurious));
    // The error status register latches on write; two writes clear it.
    write(kRegErrorStatus, 0);
    write(kRegErrorStatus, 0);

    write(kRegTimerDivide, kTimerDivideBy16);
    write(kRegLvtTimer, kLvtTimerPeriodic | static_cast<u32>(kVectorTimer));
    write(kRegTimerInitial, g_ticks_per_period);
}

u32 id() {
    const u32 raw = read(kRegId);
    return g_x2apic ? raw : raw >> 24;
}

void eoi() { write(kRegEoi, 0); }

void send_ipi(u32 apic_id, u8 vector) {
    // Fixed delivery, physical destination, edge triggered: just the vector.
    if (g_x2apic) {
        // One 64-bit MSR write; the destination is the full 32-bit id.
        cpu::wrmsr(0x830, (static_cast<u64>(apic_id) << 32) | vector);
        return;
    }
    // The ICR is two registers and a send is not atomic across them, so this
    // CPU must not be interrupted -- and sending again -- in between.
    const u64 flags = irq::save();
    while (read(kRegIcrLow) & kIcrDeliveryPending) cpu_relax();
    write(kRegIcrHigh, apic_id << 24);
    write(kRegIcrLow, vector);   // writing the low half sends
    irq::restore(flags);
}

} // namespace lapic
