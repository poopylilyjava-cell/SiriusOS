#include <drivers/usb/hid.hpp>
#include <klib/string.hpp>
#include <sirius/input.h>

namespace hid {
namespace {

// Usage pages, and the usages read from them.
constexpr u16 kPageGenericDesktop = 0x01, kPageKeyboard = 0x07, kPageButton = 0x09;
constexpr u16 kUsageX = 0x30, kUsageY = 0x31, kUsageWheel = 0x38;

// USB HID keyboard usages 0x00-0xe7 to evdev key codes (as Linux maps them,
// hid-input.c). Zero: no key.
constexpr u8 kKeyCodes[0xe8] = {
    0,   0,   0,   0,   30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,    // 00: -, a..l
    50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,  2,   3,     // 10: m..z, 1, 2
    4,   5,   6,   7,   8,   9,   10,  11,  28,  1,   14,  15,  57,  12,  13,  26,    // 20: 3..0, enter, esc, bksp, tab, space, - = [
    27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,    // 30: ] \ # ; ' ` , . / caps f1..f6
    65,  66,  67,  68,  87,  88,  99,  70,  119, 110, 102, 104, 111, 107, 109, 106,   // 40: f7..f12 sysrq scroll pause ins home pgup del end pgdn right
    105, 108, 103, 69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,    // 50: left down up numlock kp/ kp* kp- kp+ kpenter kp1..kp7
    72,  73,  82,  83,  86,  127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,   // 60: kp8 kp9 kp0 kp. 102nd compose power kp= f13..f20
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,   // 70: f21..f24 open help props front stop again undo cut copy paste find mute
    115, 114, 0,   0,   0,   121, 0,   89,  93,  124, 92,  94,  95,  0,   0,   0,     // 80: volup voldown . . . kpcomma . ro katakana yen henkan muhenkan kpjpcomma
    122, 123, 90,  91,  85,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,     // 90: hangeul hanja katakana hiragana zenkaku
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,     // a0
    0,   0,   0,   0,   0,   0,   179, 180, 0,   0,   0,   0,   0,   0,   0,   0,     // b0: kp( kp)
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,     // c0
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,     // d0
    29,  42,  56,  125, 97,  54,  100, 126,                                            // e0: lctrl lshift lalt lmeta rctrl rshift ralt rmeta
};

i32 sign_extend(u32 value, u32 bits) {
    if (bits == 0 || bits >= 32) return static_cast<i32>(value);
    const u32 sign = 1u << (bits - 1);
    return static_cast<i32>((value ^ sign) - sign);
}

// `bits` bits from `offset` bits into `data`, least significant first.
u32 extract(const u8* data, u32 offset, u32 bits) {
    u32 value = 0;
    for (u32 i = 0; i < bits; i++) {
        const u32 bit = offset + i;
        if (data[bit / 8] & (1u << (bit % 8))) value |= 1u << i;
    }
    return value;
}

struct Globals {
    u16 usage_page = 0;
    i32 logical_min = 0, logical_max = 0;
    u32 report_size = 0, report_count = 0;
    u8  report_id = 0;
};

void set_bit(u8* bits, u16 index, bool on) {
    if (on) bits[index / 8] |= static_cast<u8>(1u << (index % 8));
    else bits[index / 8] &= static_cast<u8>(~(1u << (index % 8)));
}

bool get_bit(const u8* bits, u16 index) { return bits[index / 8] & (1u << (index % 8)); }

// Ctrl, Shift, Alt and Meta, left and right.
bool is_modifier(u16 code) {
    return code == 29 || code == 42 || code == 56 || code == 125 || code == 97 || code == 54 || code == 100 || code == 126;
}

// The array value a keyboard sends in every slot when too many keys are
// held to tell which: the report says nothing about the keys.
constexpr u32 kErrorRollOver = 0x01;

} // namespace

u16 key_code(u16 usage) { return usage < sizeof(kKeyCodes) ? kKeyCodes[usage] : 0; }

bool parse(const u8* data, usize length, Descriptor& out) {
    out = Descriptor();
    Globals globals, stack[4];
    u32 depth = 0;
    // Locals: cleared after each main item.
    u16 usages[16];
    u8 usage_count = 0;
    u16 usage_min = 0, usage_max = 0;
    bool have_range = false;
    // Bit offset reached in each report, by id.
    u32 offsets[256] = {};

    for (usize i = 0; i < length;) {
        const u8 prefix = data[i++];
        if (prefix == 0xfe) {                      // a long item: skipped
            if (i + 1 >= length) return false;
            i += 2 + data[i];
            continue;
        }
        const u32 size = (prefix & 3) == 3 ? 4 : (prefix & 3);
        if (i + size > length) return false;
        u32 value = 0;
        for (u32 b = 0; b < size; b++) value |= static_cast<u32>(data[i + b]) << (8 * b);
        i += size;
        const u8 type = (prefix >> 2) & 3, tag = prefix >> 4;

        if (type == 1) {   // global
            switch (tag) {
            case 0: globals.usage_page = static_cast<u16>(value); break;
            case 1: globals.logical_min = sign_extend(value, size * 8); break;
            case 2: globals.logical_max = sign_extend(value, size * 8); break;
            case 7: globals.report_size = value; break;
            case 8: globals.report_id = static_cast<u8>(value); out.report_ids = true; break;
            case 9: globals.report_count = value; break;
            case 10: if (depth < 4) stack[depth++] = globals; break;
            case 11: if (depth > 0) globals = stack[--depth]; break;
            default: break;
            }
            // A logical maximum that only fits unsigned (0..255 written in
            // one byte) is unsigned.
            if (tag == 2 && globals.logical_min >= 0 && globals.logical_max < 0)
                globals.logical_max = static_cast<i32>(value);
        } else if (type == 2) {   // local
            if (tag == 0 && usage_count < 16) usages[usage_count++] = static_cast<u16>(value);
            else if (tag == 1) { usage_min = static_cast<u16>(value); have_range = true; }
            else if (tag == 2) { usage_max = static_cast<u16>(value); have_range = true; }
        } else if (type == 0) {   // main
            if (tag == 8) {       // Input
                const u32 bits = globals.report_size * globals.report_count;
                u32& offset = offsets[globals.report_id];
                const bool constant = value & 1;
                if (!constant && globals.report_size > 0 && globals.report_size <= 32 && out.field_count < kMaxFields) {
                    Field& f = out.fields[out.field_count++];
                    f = Field();
                    f.report_id = globals.report_id;
                    f.offset = offset;
                    f.size = static_cast<u8>(globals.report_size);
                    f.count = static_cast<u8>(globals.report_count > 255 ? 255 : globals.report_count);
                    f.usage_page = globals.usage_page;
                    memcpy(f.usages, usages, sizeof(usages));
                    f.usage_count = usage_count;
                    f.usage_min = have_range ? usage_min : (usage_count ? usages[0] : 0);
                    f.usage_max = have_range ? usage_max : (usage_count ? usages[usage_count - 1] : 0);
                    f.logical_min = globals.logical_min;
                    f.logical_max = globals.logical_max;
                    f.variable = value & 2;
                    f.relative = value & 4;
                    f.constant = false;
                }
                offset += bits;
            }
            usage_count = 0;
            have_range = false;
            usage_min = usage_max = 0;
        }
    }
    return out.field_count > 0;
}

void Translator::boot_keyboard() { m_kind = Kind::BootKeyboard; }
void Translator::boot_mouse() { m_kind = Kind::BootMouse; }

void Translator::use(const Descriptor& descriptor) {
    m_kind = Kind::Described;
    m_descriptor = descriptor;
}

bool Translator::is_keyboard() const {
    if (m_kind == Kind::BootKeyboard) return true;
    if (m_kind != Kind::Described) return false;
    for (u32 i = 0; i < m_descriptor.field_count; i++)
        if (m_descriptor.fields[i].usage_page == kPageKeyboard) return true;
    return false;
}

bool Translator::is_pointer() const {
    if (m_kind == Kind::BootMouse) return true;
    if (m_kind != Kind::Described) return false;
    for (u32 i = 0; i < m_descriptor.field_count; i++)
        if (m_descriptor.fields[i].usage_page == kPageGenericDesktop) return true;
    return false;
}

void Translator::report(const u8* data, usize length, Sink& sink) {
    bool any = false;
    u8 keys[32] = {};
    bool keyboard = false;
    bool rollover = false;
    u8 buttons = m_buttons;
    bool have_buttons = false;

    if (m_kind == Kind::BootKeyboard) {
        if (length < 3) return;
        keyboard = true;
        for (u16 bit = 0; bit < 8; bit++)
            if (data[0] & (1u << bit)) set_bit(keys, key_code(static_cast<u16>(0xe0 + bit)), true);
        for (usize i = 2; i < length && i < 8; i++) {
            if (data[i] == kErrorRollOver) rollover = true;
            if (data[i] > 3) if (const u16 code = key_code(data[i])) set_bit(keys, code, true);
        }
    } else if (m_kind == Kind::BootMouse) {
        if (length < 3) return;
        buttons = data[0] & 7;
        have_buttons = true;
        const i32 dx = static_cast<i8>(data[1]), dy = static_cast<i8>(data[2]);
        if (dx) { sink.event(SIRIUS_EV_REL, SIRIUS_REL_X, dx); any = true; }
        if (dy) { sink.event(SIRIUS_EV_REL, SIRIUS_REL_Y, dy); any = true; }
        if (length >= 4 && data[3]) { sink.event(SIRIUS_EV_REL, SIRIUS_REL_WHEEL, static_cast<i8>(data[3])); any = true; }
    } else if (m_kind == Kind::Described) {
        u8 id = 0;
        if (m_descriptor.report_ids) {
            if (length < 1) return;
            id = data[0];
            data++;
            length--;
        }
        for (u32 n = 0; n < m_descriptor.field_count; n++) {
            const Field& f = m_descriptor.fields[n];
            if (f.report_id != id || f.offset + static_cast<u32>(f.size) * f.count > length * 8) continue;
            for (u32 i = 0; i < f.count; i++) {
                const u32 raw = extract(data, f.offset + i * f.size, f.size);
                const i32 value = f.logical_min < 0 ? sign_extend(raw, f.size) : static_cast<i32>(raw);
                // The usage this value is: its own, for a variable item; the
                // value itself, for an array.
                u32 usage;
                if (f.variable) {
                    usage = f.usage_count > 1 ? f.usages[i < f.usage_count ? i : f.usage_count - 1]
                                              : static_cast<u32>(f.usage_min) + i;
                    if (f.usage_count == 1 && f.usage_min == f.usage_max) usage = f.usage_min;
                } else {
                    if (value < f.logical_min || value > f.logical_max) continue;   // nothing pressed in this slot
                    usage = static_cast<u32>(f.usage_min) + static_cast<u32>(value - f.logical_min);
                }

                if (f.usage_page == kPageKeyboard) {
                    keyboard = true;
                    if (!f.variable && usage == kErrorRollOver) rollover = true;
                    if (f.variable ? value != 0 : usage > 3)
                        if (const u16 code = key_code(static_cast<u16>(usage))) set_bit(keys, code, true);
                } else if (f.usage_page == kPageButton && f.variable) {
                    if (!have_buttons) { buttons = 0; have_buttons = true; }
                    if (usage >= 1 && usage <= 3 && value) buttons |= static_cast<u8>(1u << (usage - 1));
                } else if (f.usage_page == kPageGenericDesktop && f.variable) {
                    if (usage == kUsageX || usage == kUsageY) {
                        if (f.relative) {
                            if (value) { sink.event(SIRIUS_EV_REL, usage == kUsageX ? SIRIUS_REL_X : SIRIUS_REL_Y, value); any = true; }
                        } else if (f.logical_max > f.logical_min) {
                            const i64 scaled = static_cast<i64>(value - f.logical_min) * SIRIUS_ABS_MAX / (f.logical_max - f.logical_min);
                            sink.event(SIRIUS_EV_ABS, usage == kUsageX ? SIRIUS_ABS_X : SIRIUS_ABS_Y,
                                       static_cast<i32>(scaled < 0 ? 0 : (scaled > SIRIUS_ABS_MAX ? SIRIUS_ABS_MAX : scaled)));
                            any = true;
                        }
                    } else if (usage == kUsageWheel && f.relative && value) {
                        sink.event(SIRIUS_EV_REL, SIRIUS_REL_WHEEL, value);
                        any = true;
                    }
                }
            }
        }
    }

    if (keyboard && !rollover) {
        // Releases first, then presses: a key that moved slots between two
        // reports is neither. Modifiers go down before the keys they modify
        // and come up after them.
        for (int pass = 0; pass < 2; pass++)
            for (u16 code = 1; code < 256; code++)
                if (is_modifier(code) == (pass == 1) && get_bit(m_keys, code) && !get_bit(keys, code)) {
                    sink.event(SIRIUS_EV_KEY, code, 0);
                    any = true;
                }
        for (int pass = 0; pass < 2; pass++)
            for (u16 code = 1; code < 256; code++)
                if (is_modifier(code) == (pass == 0) && !get_bit(m_keys, code) && get_bit(keys, code)) {
                    sink.event(SIRIUS_EV_KEY, code, 1);
                    any = true;
                }
        memcpy(m_keys, keys, sizeof(keys));
    }
    if (have_buttons) {
        constexpr u16 kCodes[3] = { SIRIUS_BTN_LEFT, SIRIUS_BTN_RIGHT, SIRIUS_BTN_MIDDLE };
        for (u32 b = 0; b < 3; b++)
            if (((m_buttons ^ buttons) >> b) & 1) { sink.event(SIRIUS_EV_KEY, kCodes[b], (buttons >> b) & 1); any = true; }
        m_buttons = buttons;
    }
    if (any) sink.event(SIRIUS_EV_SYN, 0, 0);
}

} // namespace hid
