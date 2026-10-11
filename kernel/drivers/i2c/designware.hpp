#pragma once
#include <klib/types.hpp>

// Synopsys DesignWare I2C controllers, as AMD's chipsets have them (ACPI id
// AMDI0010): the bus a laptop's touchpad is usually on.
//
// Found without the firmware's AML, which would say where they are: on an
// AMD processor, the fixed addresses AMD's chipsets put them at are probed,
// and a controller is believed only if it identifies itself as DesignWare.
namespace i2c {

class DesignWare {
public:
    // Probe the controller at `physical`. False if there is none there.
    bool init(u64 physical);

    // Write `out`, then (with a repeated start) read `in_length` bytes into
    // `in`, to and from the device at 7-bit `address`. Either may be empty.
    // False if the device did not answer, or the transfer failed.
    bool transfer(u8 address, const u8* out, u32 out_length, u8* in, u32 in_length);

    u64 physical() const { return m_physical; }

private:
    volatile u32& reg(u32 offset) { return *reinterpret_cast<volatile u32*>(m_mmio + offset); }
    // A read for its effect: the clear-on-read registers.
    u32 read(u32 offset) { return reg(offset); }
    void enable(bool on);

    u8* m_mmio = nullptr;
    u64 m_physical = 0;
    u32 m_tx_depth = 8, m_rx_depth = 8;
};

// Every DesignWare controller found, by probing. Call once, from a thread
// (it sleeps). Returns how many; `out` holds that many.
u32 probe_amd(DesignWare* out, u32 capacity);

} // namespace i2c
