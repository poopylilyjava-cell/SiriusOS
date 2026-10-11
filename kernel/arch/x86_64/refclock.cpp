#include <arch/x86_64/refclock.hpp>
#include <arch/x86_64/cpu.hpp>
#include <acpi/acpi.hpp>
#include <mm/vmm.hpp>
#include <klib/print.hpp>

namespace refclock {
namespace {

enum class Source { PmTimer, Hpet, Pit };
Source g_source = Source::Pit;

// --- the ACPI PM timer --------------------------------------------------------------
volatile u32* g_pm_mmio = nullptr;
u16 g_pm_port = 0;
u32 g_pm_mask = 0xffffff;

u32 pm_read() { return (g_pm_mmio != nullptr ? *g_pm_mmio : cpu::inl(g_pm_port)) & g_pm_mask; }

// --- the HPET -----------------------------------------------------------------------
volatile u64* g_hpet = nullptr;
u64 g_hpet_period_fs = 0;    // femtoseconds per count
u64 g_hpet_mask = ~0ull;

u64 hpet_read() { return g_hpet[0xf0 / 8] & g_hpet_mask; }

// --- the PIT: channel 2, gated and polled through port 0x61 --------------------------
constexpr u64 kPitHz = 1193182;

void pit_oneshot(u16 count) {
    constexpr u16 kPortB = 0x61, kCommand = 0x43, kChannel2 = 0x42;
    u8 port_b = cpu::inb(kPortB);
    cpu::outb(kPortB, static_cast<u8>((port_b & ~0x02) | 0x01));  // gate on, speaker off
    cpu::outb(kCommand, 0xb0);  // channel 2, lobyte/hibyte, mode 0, binary
    cpu::outb(kChannel2, static_cast<u8>(count & 0xff));
    cpu::outb(kChannel2, static_cast<u8>(count >> 8));
    // Restart the count by pulsing the gate.
    port_b = cpu::inb(kPortB) & static_cast<u8>(~0x01);
    cpu::outb(kPortB, port_b);
    cpu::outb(kPortB, port_b | 0x01);
}

bool pit_expired() { return (cpu::inb(0x61) & 0x20) != 0; }

// Wait `counts` of a counter that wraps at `mask`, read by `read`.
template <typename Read>
void spin(u64 counts, u64 mask, Read read) {
    u64 last = read(), elapsed = 0;
    while (elapsed < counts) {
        __builtin_ia32_pause();
        const u64 now = read();
        elapsed += (now - last) & mask;
        last = now;
    }
}

// Does a counter move? Some firmware describes timers it has switched off.
template <typename Read>
bool moves(Read read) {
    const u64 first = read();
    for (u32 i = 0; i < 100'000; i++)
        if (read() != first) return true;
    return false;
}

bool try_pm_timer() {
    const acpi::Fadt& fadt = acpi::fadt();
    if (!fadt.pm_timer.present() || fadt.hardware_reduced) return false;
    g_pm_mask = fadt.pm_timer_32bit ? 0xffffffffu : 0xffffffu;
    if (fadt.pm_timer.space == acpi::Register::Space::Io) {
        g_pm_port = static_cast<u16>(fadt.pm_timer.address);
    } else {
        const u64 address = vmm::map_mmio(fadt.pm_timer.address & ~0xfffull, 4096);
        if (address == 0) return false;
        g_pm_mmio = reinterpret_cast<volatile u32*>(address + (fadt.pm_timer.address & 0xfff));
    }
    return moves(pm_read);
}

bool try_hpet() {
    const u64 physical = acpi::hpet_address();
    if (physical == 0) return false;
    const u64 address = vmm::map_mmio(physical, 4096);
    if (address == 0) return false;
    g_hpet = reinterpret_cast<volatile u64*>(address);
    const u64 capabilities = g_hpet[0];
    g_hpet_period_fs = capabilities >> 32;
    if (g_hpet_period_fs == 0 || g_hpet_period_fs > 100'000'000) return false;   // the spec's limit: 100 ns
    if (!(capabilities & (1u << 13))) g_hpet_mask = 0xffffffffu;               // a 32-bit counter
    g_hpet[0x10 / 8] = g_hpet[0x10 / 8] | 1;   // make sure it is counting
    return moves(hpet_read);
}

} // namespace

void init() {
    if (try_pm_timer()) g_source = Source::PmTimer;
    else if (try_hpet()) g_source = Source::Hpet;
    else g_source = Source::Pit;
    LOG_INFO("refclock: timing with the %s", name());
}

const char* name() {
    switch (g_source) {
    case Source::PmTimer: return "pm timer";
    case Source::Hpet:    return "hpet";
    default:              return "pit";
    }
}

void delay_us(u64 microseconds) {
    switch (g_source) {
    case Source::PmTimer:
        spin(microseconds * acpi::kPmTimerHz / 1'000'000 + 1, g_pm_mask, pm_read);
        return;
    case Source::Hpet:
        spin(microseconds * 1'000'000'000 / g_hpet_period_fs + 1, g_hpet_mask, hpet_read);
        return;
    case Source::Pit:
        // A 16-bit count covers at most ~54 ms per round.
        while (microseconds > 0) {
            const u64 chunk = microseconds > 50'000 ? 50'000 : microseconds;
            const u64 count = kPitHz * chunk / 1'000'000;
            pit_oneshot(static_cast<u16>(count ? count : 1));
            while (!pit_expired()) __builtin_ia32_pause();
            microseconds -= chunk;
        }
        return;
    }
}

} // namespace refclock
