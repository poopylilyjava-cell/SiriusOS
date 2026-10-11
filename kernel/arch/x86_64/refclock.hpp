#pragma once
#include <klib/types.hpp>

// A clock of known frequency, to measure time with before the TSC and the
// APIC timer have been calibrated against it -- and to calibrate them.
//
// The PC's PIT was the obvious one, and modern machines may not have it, or
// have it only half-emulated. In order of preference: the ACPI power
// management timer (3.579545 MHz, every ACPI machine with a FADT that is not
// hardware-reduced), the HPET, then the PIT.
namespace refclock {

// Choose a clock. After acpi::init(); before lapic::init_bsp().
void init();

// "pm timer", "hpet" or "pit": which one init() chose.
const char* name();

// Wait, spinning, for at least this long.
void delay_us(u64 microseconds);

} // namespace refclock
