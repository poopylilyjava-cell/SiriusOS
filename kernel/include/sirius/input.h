/* Input events, as read from /dev/input.
 *
 * The device is a stream of fixed-size records. The model and every number
 * in it are Linux's evdev: the same event types, relative axes, button codes
 * and key codes (linux/input-event-codes.h). Wayland's wl_keyboard and
 * wl_pointer speak exactly these codes, so a compositor can forward them
 * untranslated. Key codes name physical keys, not characters: turning KEY_Q
 * into 'q', 'Q' or '@' is the reader's business, and its keyboard layout's.
 * Plain C. */
#ifndef SIRIUS_INPUT_H
#define SIRIUS_INPUT_H

#include <stdint.h>

struct sirius_input_event {
    uint64_t time_ns;   /* when the kernel saw it, on CLOCK_MONOTONIC */
    uint16_t type;      /* SIRIUS_EV_* */
    uint16_t code;      /* which key, button or axis */
    int32_t  value;     /* keys: 0 released, 1 pressed, 2 auto-repeat; axes: the motion */
};

#define SIRIUS_EV_SYN  0   /* end of a group of events that happened together */
#define SIRIUS_EV_KEY  1   /* keys and mouse buttons */
#define SIRIUS_EV_REL  2   /* relative motion: mice */
#define SIRIUS_EV_ABS  3   /* absolute position, 0..SIRIUS_ABS_MAX on each axis: tablets */

#define SIRIUS_REL_X      0
#define SIRIUS_REL_Y      1   /* positive is down the screen */
#define SIRIUS_REL_WHEEL  8   /* positive is away from the user */

#define SIRIUS_ABS_X    0
#define SIRIUS_ABS_Y    1
#define SIRIUS_ABS_MAX  32767

#define SIRIUS_BTN_LEFT    0x110
#define SIRIUS_BTN_RIGHT   0x111
#define SIRIUS_BTN_MIDDLE  0x112

#define SIRIUS_KEY_ESC          1
#define SIRIUS_KEY_1            2
#define SIRIUS_KEY_2            3
#define SIRIUS_KEY_3            4
#define SIRIUS_KEY_4            5
#define SIRIUS_KEY_5            6
#define SIRIUS_KEY_6            7
#define SIRIUS_KEY_7            8
#define SIRIUS_KEY_8            9
#define SIRIUS_KEY_9           10
#define SIRIUS_KEY_0           11
#define SIRIUS_KEY_MINUS       12
#define SIRIUS_KEY_EQUAL       13
#define SIRIUS_KEY_BACKSPACE   14
#define SIRIUS_KEY_TAB         15
#define SIRIUS_KEY_Q           16
#define SIRIUS_KEY_W           17
#define SIRIUS_KEY_E           18
#define SIRIUS_KEY_R           19
#define SIRIUS_KEY_T           20
#define SIRIUS_KEY_Y           21
#define SIRIUS_KEY_U           22
#define SIRIUS_KEY_I           23
#define SIRIUS_KEY_O           24
#define SIRIUS_KEY_P           25
#define SIRIUS_KEY_LEFTBRACE   26
#define SIRIUS_KEY_RIGHTBRACE  27
#define SIRIUS_KEY_ENTER       28
#define SIRIUS_KEY_LEFTCTRL    29
#define SIRIUS_KEY_A           30
#define SIRIUS_KEY_S           31
#define SIRIUS_KEY_D           32
#define SIRIUS_KEY_F           33
#define SIRIUS_KEY_G           34
#define SIRIUS_KEY_H           35
#define SIRIUS_KEY_J           36
#define SIRIUS_KEY_K           37
#define SIRIUS_KEY_L           38
#define SIRIUS_KEY_SEMICOLON   39
#define SIRIUS_KEY_APOSTROPHE  40
#define SIRIUS_KEY_GRAVE       41
#define SIRIUS_KEY_LEFTSHIFT   42
#define SIRIUS_KEY_BACKSLASH   43
#define SIRIUS_KEY_Z           44
#define SIRIUS_KEY_X           45
#define SIRIUS_KEY_C           46
#define SIRIUS_KEY_V           47
#define SIRIUS_KEY_B           48
#define SIRIUS_KEY_N           49
#define SIRIUS_KEY_M           50
#define SIRIUS_KEY_COMMA       51
#define SIRIUS_KEY_DOT         52
#define SIRIUS_KEY_SLASH       53
#define SIRIUS_KEY_RIGHTSHIFT  54
#define SIRIUS_KEY_KPASTERISK  55
#define SIRIUS_KEY_LEFTALT     56
#define SIRIUS_KEY_SPACE       57
#define SIRIUS_KEY_CAPSLOCK    58
#define SIRIUS_KEY_F1          59
#define SIRIUS_KEY_F2          60
#define SIRIUS_KEY_F3          61
#define SIRIUS_KEY_F4          62
#define SIRIUS_KEY_F5          63
#define SIRIUS_KEY_F6          64
#define SIRIUS_KEY_F7          65
#define SIRIUS_KEY_F8          66
#define SIRIUS_KEY_F9          67
#define SIRIUS_KEY_F10         68
#define SIRIUS_KEY_NUMLOCK     69
#define SIRIUS_KEY_SCROLLLOCK  70
#define SIRIUS_KEY_KP7         71
#define SIRIUS_KEY_KP8         72
#define SIRIUS_KEY_KP9         73
#define SIRIUS_KEY_KPMINUS     74
#define SIRIUS_KEY_KP4         75
#define SIRIUS_KEY_KP5         76
#define SIRIUS_KEY_KP6         77
#define SIRIUS_KEY_KPPLUS      78
#define SIRIUS_KEY_KP1         79
#define SIRIUS_KEY_KP2         80
#define SIRIUS_KEY_KP3         81
#define SIRIUS_KEY_KP0         82
#define SIRIUS_KEY_KPDOT       83
#define SIRIUS_KEY_102ND       86   /* the extra key beside left shift on ISO keyboards */
#define SIRIUS_KEY_F11         87
#define SIRIUS_KEY_F12         88
#define SIRIUS_KEY_KPENTER     96
#define SIRIUS_KEY_RIGHTCTRL   97
#define SIRIUS_KEY_KPSLASH     98
#define SIRIUS_KEY_SYSRQ       99
#define SIRIUS_KEY_RIGHTALT   100
#define SIRIUS_KEY_HOME       102
#define SIRIUS_KEY_UP         103
#define SIRIUS_KEY_PAGEUP     104
#define SIRIUS_KEY_LEFT       105
#define SIRIUS_KEY_RIGHT      106
#define SIRIUS_KEY_END        107
#define SIRIUS_KEY_DOWN       108
#define SIRIUS_KEY_PAGEDOWN   109
#define SIRIUS_KEY_INSERT     110
#define SIRIUS_KEY_DELETE     111
#define SIRIUS_KEY_PAUSE      119
#define SIRIUS_KEY_LEFTMETA   125
#define SIRIUS_KEY_RIGHTMETA  126
#define SIRIUS_KEY_COMPOSE    127   /* the menu key */

#define SIRIUS_KEY_MAX        127

#endif
