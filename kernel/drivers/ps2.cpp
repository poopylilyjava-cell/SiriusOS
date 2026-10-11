#include <drivers/ps2.hpp>
#include <drivers/ps2_decode.hpp>
#include <drivers/input.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/ioapic.hpp>
#include <arch/x86_64/lapic.hpp>
#include <klib/print.hpp>
#include <klib/spinlock.hpp>
#include <sirius/input.h>

namespace ps2 {
namespace {

constexpr u16 kDataPort    = 0x60;
constexpr u16 kStatusPort  = 0x64;   // read
constexpr u16 kCommandPort = 0x64;   // write

constexpr u8 kStatusOutputFull = 1 << 0;   // a byte is waiting at the data port
constexpr u8 kStatusInputFull  = 1 << 1;   // the controller has not taken our last byte yet
constexpr u8 kStatusFromMouse  = 1 << 5;   // the waiting byte came from the second port

constexpr u8 kCommandReadConfig   = 0x20;
constexpr u8 kCommandWriteConfig  = 0x60;
constexpr u8 kCommandDisableMouse = 0xa7;
constexpr u8 kCommandEnableMouse  = 0xa8;
constexpr u8 kCommandSelfTest     = 0xaa;
constexpr u8 kCommandDisableKeyboard = 0xad;
constexpr u8 kCommandEnableKeyboard  = 0xae;
constexpr u8 kCommandToMouse      = 0xd4;  // the next data byte goes to the second port

constexpr u8 kConfigKeyboardIrq = 1 << 0;
constexpr u8 kConfigMouseIrq    = 1 << 1;
constexpr u8 kConfigKeyboardOff = 1 << 4;  // the port's clock is held: the device cannot send
constexpr u8 kConfigMouseOff    = 1 << 5;
constexpr u8 kConfigTranslate   = 1 << 6;  // deliver scan code set 1 whatever the keyboard speaks

constexpr u8 kDeviceAck = 0xfa;
constexpr u8 kMouseSetDefaults   = 0xf6;
constexpr u8 kMouseSetSampleRate = 0xf3;
constexpr u8 kMouseGetId         = 0xf2;
constexpr u8 kMouseEnable        = 0xf4;

constexpr u8 kIrqKeyboard = 1, kIrqMouse = 12;

Spinlock g_lock;   // the decoders, and the order bytes are taken from the port
KeyboardDecoder g_keyboard;
MouseDecoder g_mouse;
bool g_buttons[3] = {};

// The controller is slow by CPU standards; every exchange is polled with a
// bound, so absent hardware costs milliseconds rather than a hang.
bool wait_until(u8 mask, bool set) {
    for (int i = 0; i < 100'000; i++) {
        if (((cpu::inb(kStatusPort) & mask) != 0) == set) return true;
        cpu::io_wait();
    }
    return false;
}

bool send_command(u8 command) {
    if (!wait_until(kStatusInputFull, false)) return false;
    cpu::outb(kCommandPort, command);
    return true;
}

bool send_data(u8 byte) {
    if (!wait_until(kStatusInputFull, false)) return false;
    cpu::outb(kDataPort, byte);
    return true;
}

int receive() {
    if (!wait_until(kStatusOutputFull, true)) return -1;
    return cpu::inb(kDataPort);
}

void flush() {
    for (int i = 0; i < 64 && (cpu::inb(kStatusPort) & kStatusOutputFull); i++) cpu::inb(kDataPort);
}

// Send one byte to the mouse and take its acknowledgement.
bool mouse_send(u8 byte) {
    return send_command(kCommandToMouse) && send_data(byte) && receive() == kDeviceAck;
}

// Wheel mice start out pretending to be plain ones. Setting the sample rate
// to 200, 100, then 80 is the knock that unlocks the fourth packet byte; the
// device then answers the identity request with 3 instead of 0.
bool mouse_enable_wheel() {
    const u8 rates[] = { 200, 100, 80 };
    for (const u8 rate : rates)
        if (!mouse_send(kMouseSetSampleRate) || !mouse_send(rate)) return false;
    return mouse_send(kMouseGetId) && receive() == 3;
}

void report_mouse(const MousePacket& packet) {
    const bool buttons[3] = { packet.left, packet.right, packet.middle };
    const u16 codes[3] = { SIRIUS_BTN_LEFT, SIRIUS_BTN_RIGHT, SIRIUS_BTN_MIDDLE };
    bool any = false;
    if (packet.dx)    { input::post(SIRIUS_EV_REL, SIRIUS_REL_X, packet.dx); any = true; }
    if (packet.dy)    { input::post(SIRIUS_EV_REL, SIRIUS_REL_Y, packet.dy); any = true; }
    if (packet.wheel) { input::post(SIRIUS_EV_REL, SIRIUS_REL_WHEEL, packet.wheel); any = true; }
    for (int i = 0; i < 3; i++) {
        if (buttons[i] == g_buttons[i]) continue;
        g_buttons[i] = buttons[i];
        input::post(SIRIUS_EV_KEY, codes[i], buttons[i] ? 1 : 0);
        any = true;
    }
    if (any) input::post(SIRIUS_EV_SYN, 0, 0);
}

// Both interrupts land here. The controller has one output buffer for both
// devices and says whose byte is in it, so whichever interrupt arrives first
// drains everything, and the other finds nothing left.
void on_interrupt() {
    ScopedLock guard(g_lock);
    for (;;) {
        const u8 status = cpu::inb(kStatusPort);
        if (!(status & kStatusOutputFull)) break;
        const u8 byte = cpu::inb(kDataPort);

        if (status & kStatusFromMouse) {
            const MousePacket packet = g_mouse.feed(byte);
            if (packet.complete) report_mouse(packet);
        } else {
            const KeyEvent key = g_keyboard.feed(byte);
            if (key.code) {
                input::post(SIRIUS_EV_KEY, key.code, key.value);
                input::post(SIRIUS_EV_SYN, 0, 0);
            }
        }
    }
}

} // namespace

void init() {
    // Quiet both devices while the controller is configured.
    send_command(kCommandDisableKeyboard);
    send_command(kCommandDisableMouse);
    flush();

    if (!send_command(kCommandReadConfig)) { LOG_WARN("ps2: no controller"); return; }
    const int read_config = receive();
    if (read_config < 0) { LOG_WARN("ps2: the controller does not answer"); return; }
    // The byte just read says both ports are off -- they are, two commands
    // ago -- and writing it back would turn them off again after they have
    // been enabled below. Clear those bits along with the interrupts.
    u8 config = static_cast<u8>(read_config);
    config = static_cast<u8>((config & ~(kConfigKeyboardIrq | kConfigMouseIrq | kConfigKeyboardOff | kConfigMouseOff)) |
                             kConfigTranslate);
    send_command(kCommandWriteConfig);
    send_data(config);

    send_command(kCommandSelfTest);
    if (receive() != 0x55) { LOG_WARN("ps2: controller self-test failed"); return; }
    // The self-test resets some controllers; say it again.
    send_command(kCommandWriteConfig);
    send_data(config);

    send_command(kCommandEnableKeyboard);
    send_command(kCommandEnableMouse);
    flush();

    bool mouse = mouse_send(kMouseSetDefaults);
    bool wheel = false;
    if (mouse) {
        wheel = mouse_enable_wheel();
        g_mouse.set_wheel(wheel);
        mouse = mouse_send(kMouseEnable);
    }
    flush();

    interrupts::set_handler(kVectorKeyboard, on_interrupt);
    interrupts::set_handler(kVectorMouse, on_interrupt);
    const u32 cpu_id = lapic::id();
    const bool keyboard_routed = ioapic::route_isa_irq(kIrqKeyboard, kVectorKeyboard, cpu_id);
    const bool mouse_routed = mouse && ioapic::route_isa_irq(kIrqMouse, kVectorMouse, cpu_id);

    if (keyboard_routed) config |= kConfigKeyboardIrq;
    if (mouse_routed)    config |= kConfigMouseIrq;
    send_command(kCommandWriteConfig);
    send_data(config);
    // The interrupts are edge triggered: a byte that arrived while they were
    // off holds the line high, and no edge will ever announce it or anything
    // after it. Take whatever is waiting now.
    on_interrupt();

    LOG_INFO("ps2: keyboard%s%s", keyboard_routed ? "" : " (no interrupt)",
             mouse_routed ? (wheel ? ", wheel mouse" : ", mouse") : ", no mouse");
}

} // namespace ps2
