#pragma once
#include <klib/types.hpp>

// The local APIC: each CPU's private interrupt controller, and the source of
// its scheduler tick. Both the xAPIC (MMIO) and x2APIC (MSR) register
// interfaces are supported, whichever the firmware left enabled.
namespace lapic {

constexpr u32 kTimerHz = 100;  // one scheduler tick every 10 ms

// On the bootstrap processor: find and map the APIC, retire the legacy PICs,
// calibrate the timer against the PIT, then do init_cpu().
void init_bsp();

// On every CPU, the BSP included: enable the APIC and start the periodic
// timer. Interrupts must stay disabled until the scheduler can take a tick.
void init_cpu();

u32  id();
void eoi();

// Send interrupt `vector` to the CPU whose local APIC id is `apic_id`.
void send_ipi(u32 apic_id, u8 vector);



} // namespace lapic
