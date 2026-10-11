#include <drivers/i2c/i2c_hid.hpp>
#include <drivers/i2c/designware.hpp>
#include <drivers/usb/hid.hpp>
#include <drivers/input.hpp>
#include <mm/heap.hpp>
#include <sched/sched.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>

namespace i2c_hid {
namespace {

// Where touchpads answer: Elan at 0x15, Synaptics and others at 0x2c, the
// rest seen in the wild. And where their HID descriptor is, by maker.
constexpr u8 kAddresses[] = { 0x15, 0x2c, 0x10, 0x14, 0x2a, 0x20 };
constexpr u16 kDescriptorRegisters[] = { 0x0001, 0x0020, 0x0000 };
constexpr u32 kMaxDevices = 4;

struct Descriptor {        // the HID descriptor, as the device sends it
    u16 length;            // 30
    u16 version;           // 0x0100
    u16 report_length;
    u16 report_register;
    u16 input_register;
    u16 max_input_length;
    u16 output_register;
    u16 max_output_length;
    u16 command_register;
    u16 data_register;
    u16 vendor, product, revision;
    u32 reserved;
} __attribute__((packed));
static_assert(sizeof(Descriptor) == 30);

struct Device {
    i2c::DesignWare* bus;
    u8 address;
    Descriptor descriptor;
    hid::Translator translator;
    u8* input;             // max_input_length bytes
};

struct InputSink final : hid::Sink {
    void event(u16 type, u16 code, i32 value) override { input::post(type, code, value); }
};

i2c::DesignWare g_buses[6];
Device g_devices[kMaxDevices];
u32 g_device_count = 0;

bool read_register(Device& d, u16 reg, u8* out, u32 length) {
    const u8 address[2] = { static_cast<u8>(reg), static_cast<u8>(reg >> 8) };
    return d.bus->transfer(d.address, address, 2, out, length);
}

// A command: SET_POWER, RESET; opcode in the high byte, argument in the low.
bool command(Device& d, u8 opcode, u8 argument) {
    const u16 reg = d.descriptor.command_register;
    const u8 bytes[4] = { static_cast<u8>(reg), static_cast<u8>(reg >> 8), argument, opcode };
    return d.bus->transfer(d.address, bytes, 4, nullptr, 0);
}

bool start(Device& d) {
    constexpr u8 kReset = 0x01, kSetPower = 0x08, kPowerOn = 0x00;
    if (!command(d, kSetPower, kPowerOn)) return false;
    sched::sleep_ms(10);
    if (!command(d, kReset, 0)) return false;
    sched::sleep_ms(100);
    // The reset's answer is an empty report; read it out of the way.
    d.input = static_cast<u8*>(kmalloc(d.descriptor.max_input_length));
    if (d.input == nullptr) return false;
    d.bus->transfer(d.address, nullptr, 0, d.input, d.descriptor.max_input_length);

    const u16 length = d.descriptor.report_length;
    auto* report = static_cast<u8*>(kmalloc(length));
    if (report == nullptr || !read_register(d, d.descriptor.report_register, report, length)) return false;
    hid::Descriptor parsed;
    const bool ok = hid::parse(report, length, parsed);
    kfree(report);
    if (!ok) return false;
    d.translator.use(parsed);
    return d.translator.is_pointer() || d.translator.is_keyboard();
}

void probe(i2c::DesignWare& bus) {
    for (const u8 address : kAddresses) {
        for (const u16 reg : kDescriptorRegisters) {
            if (g_device_count == kMaxDevices) return;
            Device& d = g_devices[g_device_count];
            d = Device();
            d.bus = &bus;
            d.address = address;
            if (!read_register(d, reg, reinterpret_cast<u8*>(&d.descriptor), sizeof(Descriptor))) break;   // nobody at this address
            const Descriptor& h = d.descriptor;
            if (h.length != 30 || h.version != 0x0100 || h.report_length == 0 || h.report_length > 8192 ||
                h.max_input_length < 3 || h.max_input_length > 1024)
                continue;   // something answered, but not with a HID descriptor here
            LOG_INFO("i2c-hid: %#lx address %#x: %04x:%04x, HID descriptor at %#x", bus.physical(), address, h.vendor,
                     h.product, reg);
            if (!start(d)) {
                LOG_WARN("i2c-hid: %04x:%04x would not start", h.vendor, h.product);
                break;
            }
            LOG_INFO("i2c-hid: %04x:%04x is a %s, polled", h.vendor, h.product,
                     d.translator.is_pointer() ? "pointer" : "keyboard");
            g_device_count++;
            break;
        }
    }
}

void main(void*) {
    const u32 buses = i2c::probe_amd(g_buses, sizeof(g_buses) / sizeof(g_buses[0]));
    for (u32 i = 0; i < buses; i++) probe(g_buses[i]);
    if (g_device_count == 0) return;

    // No interrupt line: ask each device for a report every tick or so. An
    // empty one (length 0, or just the length field) means nothing new.
    InputSink sink;
    for (;;) {
        for (u32 i = 0; i < g_device_count; i++) {
            Device& d = g_devices[i];
            if (!d.bus->transfer(d.address, nullptr, 0, d.input, d.descriptor.max_input_length)) continue;
            const u16 length = static_cast<u16>(d.input[0] | (d.input[1] << 8));
            if (length > 2 && length <= d.descriptor.max_input_length) d.translator.report(d.input + 2, length - 2u, sink);
        }
        sched::sleep_ms(1);
    }
}

} // namespace

void init() {
    sched::spawn("i2c-hid", main, nullptr);
}

} // namespace i2c_hid
