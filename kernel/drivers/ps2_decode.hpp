#pragma once
#include <klib/types.hpp>

// Turning the byte streams of a PS/2 keyboard and mouse into events. Pure
// state machines with no hardware access: ps2.cpp feeds them from the
// interrupt handler, and the host tests feed them recorded and hostile
// sequences.
namespace ps2 {

// --- keyboard: scan code set 1 ---------------------------------------------------
// The i8042 translates whatever the keyboard speaks into set 1, where a key
// is one byte, bit 7 set on release, optionally behind an 0xe0 prefix.
struct KeyEvent {
    u16 code;    // SIRIUS_KEY_*; 0 if the byte did not complete a key
    u8  value;   // 0 released, 1 pressed, 2 auto-repeat
};

class KeyboardDecoder {
public:
    KeyEvent feed(u8 byte);

private:
    bool m_extended = false;     // an 0xe0 prefix is pending
    u8   m_pause_skip = 0;       // bytes of a Pause sequence still to drop
    u8   m_down[128 / 8] = {};   // by key code: is it held? Tells repeats from presses
};

// --- mouse -----------------------------------------------------------------------
// Three-byte packets: buttons and signs, x motion, y motion. A wheel mouse,
// once switched into its IntelliMouse mode, sends a fourth with the wheel.
struct MousePacket {
    bool complete;               // false: the byte was consumed, no packet yet
    i32  dx, dy;                 // screen convention: positive dy is down
    i32  wheel;                  // positive is away from the user
    bool left, right, middle;
};

class MouseDecoder {
public:
    void set_wheel(bool has_wheel) { m_size = has_wheel ? 4 : 3; m_count = 0; }
    MousePacket feed(u8 byte);

private:
    u8 m_bytes[4] = {};
    u8 m_count = 0;
    u8 m_size = 3;
};

} // namespace ps2
