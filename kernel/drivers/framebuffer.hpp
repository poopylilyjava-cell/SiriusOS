#pragma once
#include <klib/types.hpp>

// The linear framebuffer handed over by the bootloader, already mapped
// write-combining in the higher half. The kernel clears it to black at boot
// and then leaves it alone: user space maps it through /dev/fb0
// (drivers/fbdev.cpp) and owns every pixel from then on.
class Framebuffer {
public:
    bool valid() const { return m_pixels != nullptr; }
    u64  width()  const { return m_width; }
    u64  height() const { return m_height; }
    u64  pitch()  const { return m_pitch; }
    u16  bpp()    const { return m_bpp; }
    const u8* pixels() const { return m_pixels; }

    void init(void* address, u64 width, u64 height, u64 pitch, u16 bpp,
              u8 red_shift, u8 green_shift, u8 blue_shift);

    u32  pack(u8 r, u8 g, u8 b) const;
    void put_pixel(u64 x, u64 y, u32 colour);
    void fill_rect(u64 x, u64 y, u64 w, u64 h, u32 colour);
    void clear(u32 colour);

private:
    u8* m_pixels = nullptr;
    u64 m_width = 0, m_height = 0, m_pitch = 0;
    u16 m_bpp = 0;
    u8  m_red_shift = 16, m_green_shift = 8, m_blue_shift = 0;
};

extern Framebuffer g_framebuffer;
