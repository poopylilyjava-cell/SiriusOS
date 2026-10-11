#pragma once
#include <klib/types.hpp>

// Human Interface Devices: keyboards, mice, tablets and touchpads, whatever
// bus they come on -- USB (xhci.cpp) or I2C (i2c_hid.cpp). A device describes
// its reports in a report descriptor; this turns the descriptor into fields,
// and each report into evdev events (sirius/input.h).
//
// Pure computation over bytes, with no kernel dependencies: the host tests
// feed it real devices' descriptors and reports.
namespace hid {

// One main item of a report descriptor: `count` values of `size` bits each,
// at `offset` bits into the report (after its id byte, if it has one).
struct Field {
    u8   report_id;
    u32  offset;
    u8   size;
    u8   count;
    u16  usage_page;
    // Variable items name a usage per value: `usages`, or a range. Array
    // items carry usage numbers as their values, from usage_min upwards.
    u16  usages[16];
    u8   usage_count;
    u16  usage_min, usage_max;
    i32  logical_min, logical_max;
    bool variable;
    bool relative;
    bool constant;
};

constexpr u32 kMaxFields = 48;

struct Descriptor {
    Field fields[kMaxFields];
    u32   field_count = 0;
    bool  report_ids = false;   // every report starts with its id
};

// Parse the input items of a report descriptor. False if it is malformed
// or describes nothing this understands.
bool parse(const u8* data, usize length, Descriptor& out);

// Where events go.
class Sink {
public:
    virtual void event(u16 type, u16 code, i32 value) = 0;
};

// One device's reports, turned into events. Remembers what was held, so a
// report that drops a key or a button releases it.
class Translator {
public:
    // The boot protocol's fixed reports, for devices put into it:
    // keyboards (modifiers, reserved, six key codes) and mice (buttons, x,
    // y, and perhaps a wheel).
    void boot_keyboard();
    void boot_mouse();
    // Reports as a descriptor describes them.
    void use(const Descriptor& descriptor);

    // One input report, `length` bytes as the device sent it.
    void report(const u8* data, usize length, Sink& sink);

    bool is_keyboard() const;
    bool is_pointer() const;

private:
    enum class Kind : u8 { None, BootKeyboard, BootMouse, Described };
    Kind m_kind = Kind::None;
    Descriptor m_descriptor;
    u8 m_keys[32] = {};          // a bit per evdev key code below 256: held
    u8 m_buttons = 0;            // left, right, middle
};

// The evdev key code for a USB keyboard usage (page 7), or 0.
u16 key_code(u16 usage);

} // namespace hid
