#include <drivers/ps2_decode.hpp>
#include <sirius/input.h>

namespace ps2 {
namespace {

// Keys behind the 0xe0 prefix. Unprefixed set 1 codes need no table: Linux
// numbered its key codes after them.
u16 extended_key(u8 code) {
    switch (code) {
    case 0x1c: return SIRIUS_KEY_KPENTER;
    case 0x1d: return SIRIUS_KEY_RIGHTCTRL;
    case 0x35: return SIRIUS_KEY_KPSLASH;
    case 0x37: return SIRIUS_KEY_SYSRQ;
    case 0x38: return SIRIUS_KEY_RIGHTALT;
    case 0x47: return SIRIUS_KEY_HOME;
    case 0x48: return SIRIUS_KEY_UP;
    case 0x49: return SIRIUS_KEY_PAGEUP;
    case 0x4b: return SIRIUS_KEY_LEFT;
    case 0x4d: return SIRIUS_KEY_RIGHT;
    case 0x4f: return SIRIUS_KEY_END;
    case 0x50: return SIRIUS_KEY_DOWN;
    case 0x51: return SIRIUS_KEY_PAGEDOWN;
    case 0x52: return SIRIUS_KEY_INSERT;
    case 0x53: return SIRIUS_KEY_DELETE;
    case 0x5b: return SIRIUS_KEY_LEFTMETA;
    case 0x5c: return SIRIUS_KEY_RIGHTMETA;
    case 0x5d: return SIRIUS_KEY_COMPOSE;
    // 0x2a and 0x36: "fake shifts" some keyboards wrap around the navigation
    // keys, for the benefit of software from 1984. Not keys.
    default:   return 0;
    }
}

} // namespace

KeyEvent KeyboardDecoder::feed(u8 byte) {
    // Pause sends e1 1d 45 e1 9d c5 on press and nothing on release.
    if (m_pause_skip) { m_pause_skip--; return {}; }
    if (byte == 0xe1) { m_pause_skip = 5; m_extended = false; return { SIRIUS_KEY_PAUSE, 1 }; }
    if (byte == 0xe0) { m_extended = true; return {}; }

    const bool released = (byte & 0x80) != 0;
    const u8 scan = byte & 0x7f;
    const bool extended = m_extended;
    m_extended = false;

    // 0x54 is SysRq as Alt+PrintScreen sends it; past 0x58 (F12) plain set 1
    // has nothing a PC keyboard produces. That also rejects the keyboard's
    // 0xfa acknowledgements and 0x00/0xff error bytes.
    const u16 code = extended ? extended_key(scan) : (scan >= 1 && scan <= SIRIUS_KEY_F12 && scan != 0x54 ? scan : 0);
    if (code == 0) return {};

    u8& held = m_down[code / 8];
    const u8 bit = static_cast<u8>(1u << (code % 8));
    if (released) {
        held = static_cast<u8>(held & ~bit);
        return { code, 0 };
    }
    // The keyboard repeats a held key by sending its press again.
    const bool repeat = (held & bit) != 0;
    held |= bit;
    return { code, static_cast<u8>(repeat ? 2 : 1) };
}

MousePacket MouseDecoder::feed(u8 byte) {
    // Bit 3 of a packet's first byte is always set. If a byte was lost, this
    // is how the stream is found again: anything without it cannot start one.
    if (m_count == 0 && !(byte & 0x08)) return {};
    m_bytes[m_count++] = byte;
    if (m_count < m_size) return {};
    m_count = 0;

    const u8 status = m_bytes[0];
    MousePacket packet{};
    packet.complete = true;
    packet.left   = status & 0x01;
    packet.right  = status & 0x02;
    packet.middle = status & 0x04;
    // Motion is nine bits, two's complement: eight in the byte and the sign
    // in the status. On overflow the value is garbage; report the buttons.
    if (!(status & 0xc0)) {
        packet.dx = static_cast<i32>(m_bytes[1]) - ((status & 0x10) ? 256 : 0);
        packet.dy = -(static_cast<i32>(m_bytes[2]) - ((status & 0x20) ? 256 : 0));   // the mouse counts up as positive
    }
    if (m_size == 4) {
        // The low four bits, signed; the mouse reports rolling towards the
        // user as positive.
        const i32 z = (m_bytes[3] & 0x08) ? static_cast<i32>(m_bytes[3] & 0x0f) - 16 : static_cast<i32>(m_bytes[3] & 0x0f);
        packet.wheel = -z;
    }
    return packet;
}

} // namespace ps2
