#include <arch/x86_64/serial.hpp>
#include <arch/x86_64/cpu.hpp>
#include <klib/spinlock.hpp>

namespace {

constexpr u16 kPort = 0x3f8; // COM1

constexpr u16 kData           = 0; // DLAB=0: data
constexpr u16 kInterruptEnable= 1; // DLAB=0: interrupt enable; DLAB=1: divisor high
constexpr u16 kFifoControl    = 2;
constexpr u16 kLineControl    = 3;
constexpr u16 kModemControl   = 4;
constexpr u16 kLineStatus     = 5;

constexpr u8 kLineStatusTxEmpty = 1 << 5;

bool g_ready = false;

// Keeps one CPU's line from being shredded by another's. Acquisition is
// bounded: if the holder died mid-line (a fault inside the lock, a panic on
// another CPU), a garbled message is better than a silent hang.
Spinlock g_lock;
constexpr u32 kLockSpins = 10'000'000;

bool tx_ready() { return (cpu::inb(kPort + kLineStatus) & kLineStatusTxEmpty) != 0; }

} // namespace

bool serial_init() {
    cpu::outb(kPort + kInterruptEnable, 0x00); // no interrupts: polled output
    cpu::outb(kPort + kLineControl,     0x80); // DLAB on, to set the divisor
    cpu::outb(kPort + kData,            0x01); // divisor 1 => 115200 baud
    cpu::outb(kPort + kInterruptEnable, 0x00);
    cpu::outb(kPort + kLineControl,     0x03); // DLAB off, 8 bits, no parity, 1 stop
    cpu::outb(kPort + kFifoControl,     0xc7); // enable + clear FIFOs, 14-byte threshold
    cpu::outb(kPort + kModemControl,    0x0b); // DTR, RTS, OUT2

    // Loopback test: confirm a real UART answers before trusting it.
    cpu::outb(kPort + kModemControl, 0x1e);
    cpu::outb(kPort + kData, 0xae);
    if (cpu::inb(kPort + kData) != 0xae) {
        g_ready = false;
        return false;
    }

    cpu::outb(kPort + kModemControl, 0x0f); // back to normal operation
    g_ready = true;
    return true;
}

void serial_write_char(char c) {
    if (!g_ready) return;
    // Bounded spin: a wedged UART must not hang the kernel, least of all inside
    // a panic handler.
    for (u32 spin = 0; spin < 100000 && !tx_ready(); spin++) cpu::io_wait();
    cpu::outb(kPort, static_cast<u8>(c));
}

void serial_write(const char* s) {
    const u64 flags = irq::save();
    bool locked = false;
    for (u32 spin = 0; spin < kLockSpins && !(locked = g_lock.try_lock()); spin++) cpu_relax();

    for (; *s; s++) {
        if (*s == '\n') serial_write_char('\r'); // terminals want CRLF
        serial_write_char(*s);
    }

    if (locked) g_lock.unlock();
    irq::restore(flags);
}
