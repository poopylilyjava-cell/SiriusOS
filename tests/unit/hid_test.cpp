// Host-side tests for the HID layer: report descriptors as real devices send
// them, and the events their reports become. Descriptors come from devices,
// so they are untrusted input too: run under asan.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <drivers/usb/hid.cpp>

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

namespace {

struct Recorder final : hid::Sink {
    std::string log;
    void event(u16 type, u16 code, i32 value) override {
        char text[48];
        snprintf(text, sizeof(text), "%s%u:%u:%d", log.empty() ? "" : " ", type, code, value);
        log += text;
    }
    std::string take() { std::string out = log; log.clear(); return out; }
};

// A keyboard's report descriptor, the boot keyboard's layout as the HID
// specification's appendix gives it.
const u8 kKeyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

// QEMU's usb-tablet: three buttons, absolute 15-bit X and Y, a wheel.
const u8 kTablet[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00,
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};

// A touchpad's mouse collection, behind report id 1, beside a digitizer
// collection (report id 3) that is not read: the shape an I2C precision
// touchpad has before anyone switches it out of mouse mode.
const u8 kTouchpadMouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0,
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x42, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0xc0, 0xc0,
};

} // namespace

int main() {
    Recorder sink;

    printf("keyboard usages:\n");
    expect(hid::key_code(0x04) == 30 && hid::key_code(0x1d) == 44 && hid::key_code(0x27) == 11 && hid::key_code(0x28) == 28,
           "letters, digits and Enter map to evdev's codes");
    expect(hid::key_code(0xe0) == 29 && hid::key_code(0xe6) == 100 && hid::key_code(0x64) == 86 && hid::key_code(0x500) == 0,
           "modifiers, the 102nd key; nothing out of range");

    printf("boot keyboard:\n");
    {
        hid::Translator keyboard;
        keyboard.boot_keyboard();
        const u8 shift_a[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
        keyboard.report(shift_a, 8, sink);
        expect(sink.take() == "1:42:1 1:30:1 0:0:0", "Shift and A pressed together");
        const u8 a_b[8] = { 0x00, 0, 0x05, 0x04, 0, 0, 0, 0 };
        keyboard.report(a_b, 8, sink);
        expect(sink.take() == "1:42:0 1:48:1 0:0:0", "Shift up, B down; A held although it moved slot");
        const u8 rollover[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
        keyboard.report(rollover, 8, sink);
        expect(sink.take().empty(), "a phantom (rollover) report changes nothing");
        const u8 none[8] = {};
        keyboard.report(none, 8, sink);
        expect(sink.take() == "1:30:0 1:48:0 0:0:0", "an empty report releases everything");
    }

    printf("boot mouse:\n");
    {
        hid::Translator mouse;
        mouse.boot_mouse();
        const u8 move[4] = { 0x01, 0x05, 0xfd, 0xff };
        mouse.report(move, 4, sink);
        expect(sink.take() == "2:0:5 2:1:-3 2:8:-1 1:272:1 0:0:0", "motion, wheel and the left button");
        const u8 up[3] = { 0, 0, 0 };
        mouse.report(up, 3, sink);
        expect(sink.take() == "1:272:0 0:0:0", "and its release");
    }

    printf("described: keyboard\n");
    {
        hid::Descriptor d;
        expect(hid::parse(kKeyboard, sizeof(kKeyboard), d) && !d.report_ids && d.field_count == 2, "two input fields, no report ids");
        hid::Translator keyboard;
        keyboard.use(d);
        expect(keyboard.is_keyboard() && !keyboard.is_pointer(), "it is a keyboard");
        const u8 ctrl_c[8] = { 0x01, 0, 0x06, 0, 0, 0, 0, 0 };
        keyboard.report(ctrl_c, 8, sink);
        expect(sink.take() == "1:29:1 1:46:1 0:0:0", "Ctrl+C, from the descriptor's layout");
    }

    printf("described: tablet\n");
    {
        hid::Descriptor d;
        expect(hid::parse(kTablet, sizeof(kTablet), d), "QEMU's tablet parses");
        hid::Translator tablet;
        tablet.use(d);
        expect(tablet.is_pointer(), "it points");
        const u8 report[6] = { 0x02, 0xff, 0x3f, 0xff, 0x7f, 0x01 };
        tablet.report(report, 6, sink);
        expect(sink.take() == "3:0:16383 3:1:32767 2:8:1 1:273:1 0:0:0", "absolute X and Y, scaled; wheel; right button");
        const u8 shorter[2] = { 0, 0 };
        tablet.report(shorter, 2, sink);
        expect(sink.take() == "1:273:0 0:0:0", "a short report gives what it has room for");
    }

    printf("described: touchpad mouse collection\n");
    {
        hid::Descriptor d;
        expect(hid::parse(kTouchpadMouse, sizeof(kTouchpadMouse), d) && d.report_ids, "parses, with report ids");
        hid::Translator pad;
        pad.use(d);
        const u8 mouse[4] = { 0x01, 0x01, 0x10, 0xf0 };
        pad.report(mouse, 4, sink);
        expect(sink.take() == "2:0:16 2:1:-16 1:272:1 0:0:0", "report 1: relative motion and a click");
        const u8 digitizer[2] = { 0x03, 0x01 };
        pad.report(digitizer, 2, sink);
        expect(sink.take().empty(), "the digitizer's report is not mistaken for anything");
    }

    printf("malformed:\n");
    {
        hid::Descriptor d;
        const u8 truncated[] = { 0x05, 0x01, 0x26, 0xff };
        expect(!hid::parse(truncated, sizeof(truncated), d), "an item cut short is refused");
        std::vector<u8> long_item = { 0xfe, 0xff, 0x00 };
        expect(!hid::parse(long_item.data(), long_item.size(), d), "a long item longer than the descriptor too");
        const u8 nothing[] = { 0x05, 0x01, 0xc0 };
        expect(!hid::parse(nothing, sizeof(nothing), d), "and one with no inputs is no use");
    }

    if (g_failures) {
        printf("hid_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("hid_test: all passed\n");
    return 0;
}
