#include <drivers/framebuffer.hpp>
#include <klib/print.hpp>

Framebuffer g_framebuffer;

void Framebuffer::init(void* address, u64 width, u64 height, u64 pitch, u16 bpp,
                       u8 red_shift, u8 green_shift, u8 blue_shift) {
    m_pixels      = static_cast<u8*>(address);
    m_width       = width;
    m_height      = height;
    m_pitch       = pitch;
    m_bpp         = bpp;
    m_red_shift   = red_shift;
    m_green_shift = green_shift;
    m_blue_shift  = blue_shift;
}

u32 Framebuffer::pack(u8 r, u8 g, u8 b) const {
    // Use the shifts the bootloader reported rather than assuming 0xRRGGBB;
    // BGR framebuffers are common on real hardware.
    return (static_cast<u32>(r) << m_red_shift) |
           (static_cast<u32>(g) << m_green_shift) |
           (static_cast<u32>(b) << m_blue_shift);
}

void Framebuffer::put_pixel(u64 x, u64 y, u32 colour) {
    if (!valid() || x >= m_width || y >= m_height) return;
    // pitch is in bytes and is not always width * 4.
    auto* pixel = reinterpret_cast<u32*>(m_pixels + y * m_pitch + x * (m_bpp / 8));
    *pixel = colour;
}

void Framebuffer::fill_rect(u64 x, u64 y, u64 w, u64 h, u32 colour) {
    if (!valid()) return;
    const u64 x_end = (x + w > m_width)  ? m_width  : x + w;
    const u64 y_end = (y + h > m_height) ? m_height : y + h;

    for (u64 row = y; row < y_end; row++) {
        auto* line = reinterpret_cast<u32*>(m_pixels + row * m_pitch);
        for (u64 col = x; col < x_end; col++) line[col] = colour;
    }
}

void Framebuffer::clear(u32 colour) { fill_rect(0, 0, m_width, m_height, colour); }
