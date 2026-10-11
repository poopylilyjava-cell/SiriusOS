#pragma once
#include <acpi/fadt.hpp>
#include <acpi/madt.hpp>

// Finding the firmware's ACPI tables. Only the static tables are read -- no
// AML interpreter -- which is enough to learn how interrupts are wired, which
// timers there are, and how to switch the machine off.
namespace acpi {

// Locate the tables from the bootloader's RSDP and parse the MADT, the FADT
// and the HPET table. Without usable tables the PC defaults stand: one I/O
// APIC at 0xfec00000, every ISA IRQ on the GSI of its own number, no ACPI
// timers and no power control.
void init();

const Madt& madt();
const Fadt& fadt();
// The HPET's registers, physical; 0 if there is none.
u64 hpet_address();

// Switch the machine off (ACPI S5). Returns only if it could not.
void power_off();
// Restart it: the FADT's reset register, then the keyboard controller's
// reset line, then a triple fault. Does not return.
[[noreturn]] void reboot();

} // namespace acpi
