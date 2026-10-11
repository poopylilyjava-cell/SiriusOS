#include <drivers/fbdev.hpp>
#include <drivers/fbconsole.hpp>
#include <boot/requests.hpp>
#include <fs/devfs.hpp>
#include <mm/memory.hpp>
#include <mm/usercopy.hpp>
#include <klib/print.hpp>
#include <sirius/errno.h>
#include <sirius/fb.h>

namespace fbdev {
namespace {

class FramebufferDevice final : public vfs::Vnode {
public:
    FramebufferDevice(vfs::Filesystem* fs, const limine_framebuffer& fb)
        : Vnode(fs, vfs::NodeType::CharDevice, 4),
          // The bootloader reports where it mapped the framebuffer, in the
          // direct map; the physical address is that minus the map's offset.
          m_physical(mm::virt_to_phys(fb.address)) {
        m_info = {};
        m_info.width  = static_cast<u32>(fb.width);
        m_info.height = static_cast<u32>(fb.height);
        m_info.pitch  = static_cast<u32>(fb.pitch);
        m_info.bpp    = fb.bpp;
        m_info.size   = fb.pitch * fb.height;
        m_info.red_shift   = fb.red_mask_shift;   m_info.red_size   = fb.red_mask_size;
        m_info.green_shift = fb.green_mask_shift; m_info.green_size = fb.green_mask_size;
        m_info.blue_shift  = fb.blue_mask_shift;  m_info.blue_size  = fb.blue_mask_size;
    }

    // Pixels are reached by mapping, not by read and write: a system call
    // per rectangle is no way to draw.
    i64 read(u64, void*, u64) override { return -SIRIUS_EINVAL; }
    i64 write(u64, const void*, u64) override { return -SIRIUS_EINVAL; }

    int ioctl(u64 request, u64 argument) override {
        if (request != SIRIUS_FB_GET_INFO) return -SIRIUS_ENOTTY;
        fbconsole::release();   // whoever asks is about to draw
        return usercopy::copy_to(argument, &m_info, sizeof(m_info));
    }

    i64 physical_page(u64 offset, bool& write_combining) override {
        if (offset % mm::kPageSize || offset >= mm::align_up(m_info.size, mm::kPageSize)) return -SIRIUS_EINVAL;
        write_combining = true;
        fbconsole::release();   // its pixels are user space's from now
        return static_cast<i64>(m_physical + offset);
    }

private:
    u64 m_physical;
    sirius_fb_info m_info;
};

} // namespace

void init() {
    if (g_framebuffer_request.response == nullptr || g_framebuffer_request.response->framebuffer_count == 0) return;
    const limine_framebuffer& fb = *g_framebuffer_request.response->framebuffers[0];
    if (fb.bpp != 32) {
        LOG_WARN("fbdev: %u bits per pixel is not supported; no /dev/fb0", fb.bpp);
        return;
    }
    devfs::add("fb0", new FramebufferDevice(devfs::instance(), fb));
    LOG_INFO("fbdev: /dev/fb0 is %lux%lu, %lu KiB", fb.width, fb.height, fb.pitch * fb.height / 1024);
}

} // namespace fbdev
