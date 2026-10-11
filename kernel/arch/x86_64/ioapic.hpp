#pragma once
#include <klib/types.hpp>

// The I/O APIC: where device interrupt lines enter, and are turned into
// vectors delivered to a CPU's local APIC. The legacy PICs it replaces stay
// masked (lapic.cpp).
namespace ioapic {

// Map the I/O APICs the ACPI tables describe and mask every input. Needs
// acpi::init().
void init();

// Deliver ISA interrupt `irq` (1 for the keyboard, 12 for the mouse, ...) as
// `vector` to the CPU with local APIC id `apic_id`, honouring whatever the
// firmware says about where the line is really wired and how it signals.
// Returns false if no I/O APIC serves it.
bool route_isa_irq(u8 irq, u8 vector, u32 apic_id);

} // namespace ioapic
