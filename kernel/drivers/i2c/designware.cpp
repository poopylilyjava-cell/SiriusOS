#include <drivers/i2c/designware.hpp>
#include <arch/x86_64/clock.hpp>
#include <arch/x86_64/refclock.hpp>
#include <arch/x86_64/cpu.hpp>
#include <mm/vmm.hpp>
#include <sched/sched.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>

namespace i2c {
namespace {

// Registers.
constexpr u32 kControl = 0x00, kTarget = 0x04, kDataCommand = 0x10, kStandardHigh = 0x14, kStandardLow = 0x18,
              kFastHigh = 0x1c, kFastLow = 0x20, kInterruptMask = 0x30, kRawInterrupts = 0x34,
              kClearInterrupts = 0x40, kClearAbort = 0x54, kEnable = 0x6c, kStatus = 0x70, kRxLevel = 0x78,
              kAbortSource = 0x80, kEnableStatus = 0x9c, kParameters = 0xf4, kComponentType = 0xfc;
constexpr u32 kDesignWare = 0x44570140;   // "DW", and the I2C component's number
constexpr u32 kRead = 1u << 8, kStop = 1u << 9, kRestart = 1u << 10;
constexpr u32 kAbort = 1u << 6;            // in the raw interrupt status
constexpr u32 kActivity = 1u << 0, kMasterActivity = 1u << 5;

// Where AMD's chipsets put their I2C controllers (I2C0 to I2C5).
constexpr u64 kAmdBases[] = { 0xfedc2000, 0xfedc3000, 0xfedc4000, 0xfedc5000, 0xfedc6000, 0xfedcb000 };

bool is_amd() {
    const cpu::CpuidResult r = cpu::cpuid(0);
    return r.ebx == 0x68747541 && r.edx == 0x69746e65 && r.ecx == 0x444d4163;   // "AuthenticAMD"
}

} // namespace

void DesignWare::enable(bool on) {
    reg(kEnable) = on ? 1 : 0;
    for (u32 i = 0; i < 1000 && ((reg(kEnableStatus) & 1) != (on ? 1u : 0u)); i++) refclock::delay_us(25);
}

bool DesignWare::init(u64 physical) {
    const u64 mapped = vmm::map_mmio(physical, 4096);
    if (mapped == 0) return false;
    m_mmio = reinterpret_cast<u8*>(mapped);
    m_physical = physical;
    if (reg(kComponentType) != kDesignWare) return false;

    const u32 parameters = reg(kParameters);
    m_tx_depth = ((parameters >> 16) & 0xff) + 1;
    m_rx_depth = ((parameters >> 8) & 0xff) + 1;

    enable(false);
    // The firmware has set the clock counts for the bus's speed, if it used
    // the bus; keep them. Otherwise, standard mode (100 kHz) at the 150 MHz
    // AMD clocks these controllers with, which every device supports.
    u32 control = reg(kControl);
    const bool firmware_fast = ((control >> 1) & 3) == 2 && reg(kFastHigh) != 0 && reg(kFastLow) != 0;
    if (!firmware_fast) {
        if (reg(kStandardHigh) == 0 || reg(kStandardLow) == 0) {
            reg(kStandardHigh) = 600;
            reg(kStandardLow) = 705;
        }
        control = (control & ~6u) | (1u << 1);
    }
    // Master, with repeated starts; the slave half off.
    reg(kControl) = (control & ~0x3f9u) | 1u | (1u << 5) | (1u << 6);
    reg(kInterruptMask) = 0;   // polled
    read(kClearInterrupts);
    return true;
}

bool DesignWare::transfer(u8 address, const u8* out, u32 out_length, u8* in, u32 in_length) {
    if (out_length + in_length == 0) return true;
    enable(false);
    reg(kTarget) = address;
    read(kClearInterrupts);
    enable(true);

    const u64 deadline = clock::now_ns() + 50'000'000;   // 50 ms: plenty at 100 kHz for what is sent
    u32 sent = 0, requested = 0, received = 0;
    const u32 total = out_length + in_length;
    bool failed = false;
    while (received < in_length || sent + requested < total) {
        if (reg(kRawInterrupts) & kAbort) { failed = true; break; }
        if (clock::now_ns() >= deadline) { failed = true; break; }
        // Queue what the FIFOs have room for: bytes to write, then read
        // requests, no more of those outstanding than the receive FIFO holds.
        while (sent + requested < total && (reg(kStatus) & (1u << 1))) {   // transmit FIFO not full
            const u32 position = sent + requested;
            const bool last = position == total - 1;
            u32 command;
            if (position < out_length) {
                command = out[position];
                sent++;
            } else {
                if (requested - received >= m_rx_depth) break;
                command = kRead | (position == out_length && out_length ? kRestart : 0);
                requested++;
            }
            if (last) command |= kStop;
            reg(kDataCommand) = command;
        }
        while (received < requested && reg(kRxLevel) > 0) in[received++] = static_cast<u8>(reg(kDataCommand));
        __builtin_ia32_pause();
    }
    if (failed) {
        if (reg(kRawInterrupts) & kAbort) {
            read(kAbortSource);
            read(kClearAbort);
        }
        enable(false);
        return false;
    }
    // Let the stop go out before the next transfer changes the target.
    for (u32 i = 0; i < 1000 && (reg(kStatus) & (kMasterActivity | kActivity)); i++) refclock::delay_us(10);
    return true;
}

u32 probe_amd(DesignWare* out, u32 capacity) {
    if (!is_amd()) return 0;
    u32 count = 0;
    for (const u64 base : kAmdBases) {
        if (count == capacity) break;
        if (out[count].init(base)) {
            LOG_INFO("i2c: DesignWare controller at %#lx", base);
            count++;
        }
    }
    return count;
}

} // namespace i2c
