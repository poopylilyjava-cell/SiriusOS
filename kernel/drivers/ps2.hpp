#pragma once

// The PS/2 controller (i8042), its keyboard and its mouse. Key presses and
// mouse motion become input events (drivers/input.hpp).
namespace ps2 {

// Initialise the controller and both devices and route their interrupts.
// Needs the I/O APIC and the input queue. A missing device is logged and
// skipped: machines without PS/2 hardware exist, and boot regardless.
void init();

} // namespace ps2
