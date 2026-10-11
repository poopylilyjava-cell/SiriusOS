#pragma once

// Text on the framebuffer: where the kernel's messages go on a machine with
// no serial port to read them from -- a laptop.
//
// Off by default: the screen stays black until user space draws. With
// console=fb on the command line it shows the kernel log as it is written,
// until user space takes the display. A panic always takes the screen back
// and shows itself there.
namespace fbconsole {

// Start, after the framebuffer is set up. `mirror` shows the log from now.
void init(bool mirror);

// Text written to the log or the console device; shown if the console is
// showing anything.
void write(const char* text);

// User space has mapped the display: hands off.
void release();

// The kernel is going down: clear the screen to a panic's colours and show
// everything from here on, whoever owned the display. Safe to call with
// interrupts off and other CPUs stopped.
void take_over();

} // namespace fbconsole
