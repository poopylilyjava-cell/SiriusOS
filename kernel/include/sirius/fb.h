/* The framebuffer device, /dev/fb0.
 *
 * ioctl(fd, SIRIUS_FB_GET_INFO, &info) describes the display; mmap(NULL,
 * info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) maps its pixels.
 * The mapping is write-combining: fast to fill, very slow to read back, so
 * draw somewhere else and copy finished pixels in. Plain C. */
#ifndef SIRIUS_FB_H
#define SIRIUS_FB_H

#include <stdint.h>

struct sirius_fb_info {
    uint32_t width;          /* pixels */
    uint32_t height;
    uint32_t pitch;          /* bytes from one row to the next; may exceed width * bpp / 8 */
    uint32_t bpp;            /* bits per pixel */
    uint64_t size;           /* bytes to mmap: pitch * height */
    uint8_t  red_shift;      /* bit position of each channel in a pixel */
    uint8_t  red_size;       /* and its width in bits */
    uint8_t  green_shift;
    uint8_t  green_size;
    uint8_t  blue_shift;
    uint8_t  blue_size;
    uint8_t  reserved[2];
};

#define SIRIUS_FB_GET_INFO  0x4600   /* (struct sirius_fb_info*) -> 0 */

#endif
