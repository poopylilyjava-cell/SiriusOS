// Host-side tests for the PS/2 byte-stream decoders. The bytes are what a
// real keyboard and mouse send; the hostile cases are what a flaky cable or
// a lost interrupt make of them.

#include <cstdio>
#include <initializer_list>
#include <vector>

#include <drivers/ps2_decode.cpp>

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

namespace {

struct Key { unsigned code, value; bool operator==(const Key&) const = default; };

// Every key event a byte sequence produces, in order.
std::vector<Key> keys(ps2::KeyboardDecoder& decoder, std::initializer_list<int> bytes) {
    std::vector<Key> out;
    for (int byte : bytes) {
        const ps2::KeyEvent event = decoder.feed(static_cast<u8>(byte));
        if (event.code) out.push_back({ event.code, event.value });
    }
    return out;
}

// The last complete packet a byte sequence produces, and how many there were.
ps2::MousePacket packets(ps2::MouseDecoder& decoder, std::initializer_list<int> bytes, int* count = nullptr) {
    ps2::MousePacket last{};
    int n = 0;
    for (int byte : bytes) {
        const ps2::MousePacket packet = decoder.feed(static_cast<u8>(byte));
        if (packet.complete) { last = packet; n++; }
    }
    if (count) *count = n;
    return last;
}

} // namespace

int main() {
    using V = std::vector<Key>;

    printf("keyboard:\n");
    {
        ps2::KeyboardDecoder decoder;
        expect(keys(decoder, { 0x1e, 0x9e }) == V{ { SIRIUS_KEY_A, 1 }, { SIRIUS_KEY_A, 0 } }, "a key is pressed and released");
        expect(keys(decoder, { 0x01, 0x81, 0x58, 0xd8 }) ==
               V{ { SIRIUS_KEY_ESC, 1 }, { SIRIUS_KEY_ESC, 0 }, { SIRIUS_KEY_F12, 1 }, { SIRIUS_KEY_F12, 0 } },
               "the first and last plain scan codes map to their key codes");
        expect(keys(decoder, { 0x2a, 0x10, 0x90, 0xaa }) ==
               V{ { SIRIUS_KEY_LEFTSHIFT, 1 }, { SIRIUS_KEY_Q, 1 }, { SIRIUS_KEY_Q, 0 }, { SIRIUS_KEY_LEFTSHIFT, 0 } },
               "modifiers are keys like any other");
        expect(keys(decoder, { 0x20, 0x20, 0x20, 0xa0, 0x20 }) ==
               V{ { SIRIUS_KEY_D, 1 }, { SIRIUS_KEY_D, 2 }, { SIRIUS_KEY_D, 2 }, { SIRIUS_KEY_D, 0 }, { SIRIUS_KEY_D, 1 } },
               "a held key's repeated presses are auto-repeats, until it is released");
        keys(decoder, { 0xa0 });
    }
    {
        ps2::KeyboardDecoder decoder;
        expect(keys(decoder, { 0xe0, 0x48, 0xe0, 0xc8 }) == V{ { SIRIUS_KEY_UP, 1 }, { SIRIUS_KEY_UP, 0 } },
               "an e0 prefix selects the navigation keys");
        expect(keys(decoder, { 0x48, 0xc8 }) == V{ { SIRIUS_KEY_KP8, 1 }, { SIRIUS_KEY_KP8, 0 } },
               "and without it the same code is the keypad");
        expect(keys(decoder, { 0xe0, 0x1d, 0xe0, 0x38, 0xe0, 0x1c, 0xe0, 0x5b }) ==
               V{ { SIRIUS_KEY_RIGHTCTRL, 1 }, { SIRIUS_KEY_RIGHTALT, 1 }, { SIRIUS_KEY_KPENTER, 1 }, { SIRIUS_KEY_LEFTMETA, 1 } },
               "the right-hand modifiers, keypad enter and the meta key are prefixed");
        expect(keys(decoder, { 0xe0, 0x2a, 0xe0, 0x47, 0xe0, 0xc7, 0xe0, 0xaa }) ==
               V{ { SIRIUS_KEY_HOME, 1 }, { SIRIUS_KEY_HOME, 0 } }, "fake shifts around a navigation key are dropped");
        expect(keys(decoder, { 0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5, 0x1e }) == V{ { SIRIUS_KEY_PAUSE, 1 }, { SIRIUS_KEY_A, 1 } },
               "the six-byte Pause sequence is one key, and the stream continues after it");
    }
    {
        ps2::KeyboardDecoder decoder;
        expect(keys(decoder, { 0xfa, 0x00, 0xff, 0xfe, 0x59, 0x7f, 0xe0, 0x7e }).empty(),
               "acknowledgements, error bytes and codes no key sends produce nothing");
        expect(keys(decoder, { 0xe0, 0xfa, 0x1e }) == V{ { SIRIUS_KEY_A, 1 } }, "a prefix does not outlive the byte after it");
    }

    printf("mouse:\n");
    {
        ps2::MouseDecoder decoder;
        ps2::MousePacket p = packets(decoder, { 0x08, 5, 3 });
        expect(p.complete && p.dx == 5 && p.dy == -3 && !p.left && !p.right && !p.middle && p.wheel == 0,
               "three bytes make a packet; moving the mouse up is negative y");
        p = packets(decoder, { 0x08 | 0x10 | 0x20, 0xfe, 0xf6 });
        expect(p.complete && p.dx == -2 && p.dy == 10, "the sign bits extend the motion to nine bits");
        p = packets(decoder, { 0x08 | 0x10, 0x00, 0x00 });
        expect(p.complete && p.dx == -256, "down to -256");
        p = packets(decoder, { 0x08 | 0x01 | 0x04, 0, 0 });
        expect(p.complete && p.left && !p.right && p.middle, "buttons are reported");
        p = packets(decoder, { 0x08 | 0x40 | 0x02, 0x7f, 0x7f });
        expect(p.complete && p.dx == 0 && p.dy == 0 && p.right, "an overflowed packet moves nothing but keeps its buttons");
    }
    {
        ps2::MouseDecoder decoder;
        int count = 0;
        // A stray byte first: it lacks the always-set bit, so it cannot start a packet.
        const ps2::MousePacket p = packets(decoder, { 0x00, 0x04, 0x09, 7, 0 }, &count);
        expect(count == 1 && p.dx == 7 && p.left, "bytes that cannot start a packet are skipped until one can");
    }
    {
        ps2::MouseDecoder decoder;
        decoder.set_wheel(true);
        int count = 0;
        ps2::MousePacket p = packets(decoder, { 0x08, 1, 1 }, &count);
        expect(count == 0, "a wheel mouse's packet needs a fourth byte");
        p = packets(decoder, { 0x01 }, &count);
        expect(count == 1 && p.wheel == -1 && p.dx == 1, "rolling towards the user scrolls down");
        p = packets(decoder, { 0x08, 0, 0, 0x0f });
        expect(p.complete && p.wheel == 1, "and away from the user scrolls up");
        p = packets(decoder, { 0x08, 0, 0, 0x00, 0x08, 2, 0, 0x00 }, &count);
        expect(count == 2 && p.dx == 2 && p.wheel == 0, "consecutive packets stay in step");
    }

    if (g_failures) {
        printf("ps2_decode_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("ps2_decode_test: all passed\n");
    return 0;
}
