#pragma once

// USB, through xHCI host controllers: what a laptop's built-in keyboard is
// on, and every USB keyboard and mouse. Each controller gets a kernel thread
// that brings up the devices on its root ports and turns their HID reports
// into input events (drivers/usb/hid.hpp, drivers/input.hpp).
//
// What there is: devices on root ports, at any speed; hot plugging; HID
// keyboards, mice and tablets. What there is not yet: hubs (a device behind
// one is not seen), and any class of device but HID.
namespace xhci {

// Find every xHCI controller and start it. After input::init().
void init();

} // namespace xhci
