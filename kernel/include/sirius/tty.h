/* Terminals: the pseudo-terminal's ioctl requests and the records they
 * carry. Shared by the kernel and user space, like the other ABI headers.
 *
 * The numbers and layouts are Linux's, because the C library's are
 * (ADR 0007): mlibc passes a struct termios and a TIOC* request straight
 * through to the kernel. */
#ifndef SIRIUS_TTY_H
#define SIRIUS_TTY_H

#include <stdint.h>

/* ioctl requests on a terminal (either end of a pseudo-terminal). */
#define SIRIUS_TCGETS     0x5401  /* struct sirius_termios*: get the settings       */
#define SIRIUS_TCSETS     0x5402  /* const struct sirius_termios*: set them now     */
#define SIRIUS_TCSETSW    0x5403  /* ...once output has drained (it always has)     */
#define SIRIUS_TCSETSF    0x5404  /* ...and discard unread input                    */
#define SIRIUS_TCFLSH     0x540B  /* 0 input, 1 output, 2 both: discard             */
#define SIRIUS_TIOCSCTTY  0x540E  /* make it the caller's controlling terminal      */
#define SIRIUS_TIOCGPGRP  0x540F  /* int32_t*: the foreground process group         */
#define SIRIUS_TIOCSPGRP  0x5410  /* const int32_t*: set it                         */
#define SIRIUS_TIOCGWINSZ 0x5413  /* struct sirius_winsize*: get the size           */
#define SIRIUS_TIOCSWINSZ 0x5414  /* const struct sirius_winsize*: set it           */
#define SIRIUS_FIONREAD   0x541B  /* int32_t*: bytes ready to read                  */
#define SIRIUS_TIOCNOTTY  0x5422  /* give up the controlling terminal               */
#define SIRIUS_TIOCGSID   0x5429  /* int32_t*: the session it controls              */

/* struct termios as the C library lays it out: 60 bytes. */
#define SIRIUS_NCCS 32
struct sirius_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[SIRIUS_NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct sirius_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* The settings the kernel understands; every other bit is kept and ignored. */
#define SIRIUS_VINTR  0
#define SIRIUS_VQUIT  1
#define SIRIUS_VERASE 2
#define SIRIUS_VKILL  3
#define SIRIUS_VEOF   4
#define SIRIUS_VTIME  5
#define SIRIUS_VMIN   6
#define SIRIUS_VSUSP  10
#define SIRIUS_VEOL   11
#define SIRIUS_VWERASE 14

#define SIRIUS_IGNCR  0000200   /* c_iflag */
#define SIRIUS_ICRNL  0000400
#define SIRIUS_INLCR  0000100
#define SIRIUS_OPOST  0000001   /* c_oflag */
#define SIRIUS_ONLCR  0000004
#define SIRIUS_ISIG   0000001   /* c_lflag */
#define SIRIUS_ICANON 0000002
#define SIRIUS_ECHO   0000010
#define SIRIUS_ECHOE  0000020
#define SIRIUS_ECHOK  0000040
#define SIRIUS_ECHONL 0000100
#define SIRIUS_ECHOCTL 0001000
#define SIRIUS_IEXTEN 0100000
#define SIRIUS_CS8    0000060   /* c_cflag */
#define SIRIUS_CREAD  0000200
#define SIRIUS_B38400 0000017

#endif
