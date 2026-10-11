#pragma once

// HID over I2C: a laptop's touchpad, as Microsoft's specification has it.
//
// The firmware's AML says which I2C bus a touchpad is on, at what address,
// where its HID descriptor is, and which GPIO is its interrupt line; there
// is no AML interpreter, so a kernel thread probes for it instead -- the
// addresses touchpads use, on every DesignWare bus (designware.hpp) -- and
// polls it. Before anyone switches it into precision-touchpad mode, a
// touchpad reports as a mouse, which hid.hpp understands.
namespace i2c_hid {

// Start the probe, in a thread of its own. After input::init().
void init();

} // namespace i2c_hid
