#pragma once

// /dev/fb0: the bootloader's framebuffer, for user space to describe
// (ioctl) and map (mmap). See sirius/fb.h.
namespace fbdev {

// Publish the device, if the bootloader provided a framebuffer.
void init();

} // namespace fbdev
