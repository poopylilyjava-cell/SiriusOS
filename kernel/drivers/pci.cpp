#include <mm/vmm.hpp>
#include <drivers/pci.hpp>
#include <arch/x86_64/cpu.hpp>
#include <klib/print.hpp>
#include <klib/spinlock.hpp>

namespace pci {
namespace {

constexpr u16 kConfigAddress = 0xcf8;
constexpr u16 kConfigData    = 0xcfc;

constexpr u8 kRegVendor     = 0x00;
constexpr u8 kRegCommand    = 0x04;
constexpr u8 kRegClass      = 0x08;  // revision, prog-if, subclass, class
constexpr u8 kRegHeaderType = 0x0e;
constexpr u8 kRegBar0       = 0x10;

constexpr u16 kCommandMemory       = 1 << 1;
constexpr u16 kCommandBusMaster    = 1 << 2;
constexpr u16 kCommandIntxDisable  = 1 << 10;

constexpr u32 kMaxDevices = 64;

// The address/data port pair is one shared register: a second CPU writing the
// address between our write and read would redirect it.
Spinlock g_lock;
Device g_devices[kMaxDevices];
u32 g_count = 0;

u32 address_for(u8 bus, u8 slot, u8 function, u8 offset) {
    return (1u << 31) | (static_cast<u32>(bus) << 16) | (static_cast<u32>(slot) << 11) |
           (static_cast<u32>(function) << 8) | (offset & 0xfc);
}

u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }

u32 raw_read(u8 bus, u8 slot, u8 function, u8 offset) {
    ScopedLock guard(g_lock);
    outl(kConfigAddress, address_for(bus, slot, function, offset));
    return inl(kConfigData);
}

void raw_write(u8 bus, u8 slot, u8 function, u8 offset, u32 value) {
    ScopedLock guard(g_lock);
    outl(kConfigAddress, address_for(bus, slot, function, offset));
    outl(kConfigData, value);
}

const char* class_name(u8 class_code, u8 subclass) {
    switch (class_code) {
    case 0x01:
        if (subclass == 0x06) return "SATA controller";
        if (subclass == 0x08) return "NVMe controller";
        return "storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x06:
        if (subclass == 0x00) return "host bridge";
        if (subclass == 0x01) return "ISA bridge";
        if (subclass == 0x04) return "PCI bridge";
        return "bridge";
    case 0x0c:
        if (subclass == 0x03) return "USB controller";
        if (subclass == 0x05) return "SMBus controller";
        return "serial bus controller";
    default: return "device";
    }
}

void probe(u8 bus, u8 slot, u8 function) {
    const u32 id = raw_read(bus, slot, function, kRegVendor);
    if ((id & 0xffff) == 0xffff) return;

    const u32 cls = raw_read(bus, slot, function, kRegClass);
    Device device{
        .bus = bus, .slot = slot, .function = function,
        .vendor_id = static_cast<u16>(id & 0xffff),
        .device_id = static_cast<u16>(id >> 16),
        .class_code = static_cast<u8>(cls >> 24),
        .subclass   = static_cast<u8>(cls >> 16),
        .prog_if    = static_cast<u8>(cls >> 8),
    };

    LOG_INFO("pci: %02x:%02x.%u %04x:%04x class %02x.%02x.%02x %s",
             bus, slot, function, device.vendor_id, device.device_id,
             device.class_code, device.subclass, device.prog_if,
             class_name(device.class_code, device.subclass));

    if (g_count == kMaxDevices) { LOG_WARN("pci: device table full"); return; }
    g_devices[g_count++] = device;
}

} // namespace

void init() {
    // Brute force over every bus: simpler than following bridges, and a
    // missing function answers 0xffff quickly.
    for (u32 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            const u8 b = static_cast<u8>(bus);
            if ((raw_read(b, slot, 0, kRegVendor) & 0xffff) == 0xffff) continue;
            const bool multifunction = (raw_read(b, slot, 0, kRegHeaderType & 0xfc) >> 16) & 0x80;
            for (u8 function = 0; function < (multifunction ? 8 : 1); function++) probe(b, slot, function);
        }
    }
    LOG_INFO("pci: %u functions", g_count);
}

u32 count() { return g_count; }
const Device& at(u32 index) { return g_devices[index]; }

const Device* find_class(u8 class_code, u8 subclass, u8 prog_if, u32 index) {
    for (u32 i = 0; i < g_count; i++) {
        const Device& d = g_devices[i];
        if (d.class_code == class_code && d.subclass == subclass && d.prog_if == prog_if) {
            if (index == 0) return &d;
            index--;
        }
    }
    return nullptr;
}

u32 read32(const Device& d, u8 offset) { return raw_read(d.bus, d.slot, d.function, offset); }

u16 read16(const Device& d, u8 offset) {
    return static_cast<u16>(read32(d, offset & 0xfc) >> ((offset & 2) * 8));
}

void write32(const Device& d, u8 offset, u32 value) { raw_write(d.bus, d.slot, d.function, offset, value); }

void write16(const Device& d, u8 offset, u16 value) {
    const u8 aligned = offset & 0xfc;
    const u32 shift = (offset & 2) * 8;
    u32 word = read32(d, aligned);
    word = (word & ~(0xffffu << shift)) | (static_cast<u32>(value) << shift);
    write32(d, aligned, word);
}

Bar bar(const Device& d, u32 index) {
    Bar result{};
    if (index > 5) return result;
    const u8 offset = static_cast<u8>(kRegBar0 + index * 4);
    const u32 low = read32(d, offset);

    // Sizing writes all ones to the BAR, which would move a live decoder.
    // Switch decoding off around it.
    const u16 command = read16(d, kRegCommand);
    write16(d, kRegCommand, command & static_cast<u16>(~0x3));

    if (low & 1) {
        result.io = true;
        result.address = low & ~0x3u;
        write32(d, offset, 0xffffffff);
        result.size = (~(read32(d, offset) & ~0x3u) + 1) & 0xffff;
        write32(d, offset, low);
    } else {
        const bool is64 = ((low >> 1) & 0x3) == 0x2;
        result.prefetchable = (low & 0x8) != 0;
        u64 address = low & ~0xfULL;
        u64 mask;

        write32(d, offset, 0xffffffff);
        mask = read32(d, offset) & ~0xfULL;
        write32(d, offset, low);

        if (is64) {
            const u32 high = read32(d, offset + 4);
            address |= static_cast<u64>(high) << 32;
            write32(d, offset + 4, 0xffffffff);
            mask |= static_cast<u64>(read32(d, offset + 4)) << 32;
            write32(d, offset + 4, high);
        } else {
            mask |= 0xffffffff00000000ULL;
        }
        result.address = address;
        result.size = ~mask + 1;
    }

    write16(d, kRegCommand, command);
    return result;
}

void enable_dma(const Device& d) {
    const u16 command = read16(d, kRegCommand);
    write16(d, kRegCommand, command | kCommandMemory | kCommandBusMaster | kCommandIntxDisable);
}

u8 find_capability(const Device& d, u8 id) {
    if (!(read16(d, 0x06) & 0x10)) return 0;   // status: no capabilities list
    u8 offset = static_cast<u8>(read32(d, 0x34) & 0xfc);
    for (u32 guard = 0; offset != 0 && guard < 48; guard++) {
        const u32 header = read32(d, offset);
        if ((header & 0xff) == id) return offset;
        offset = static_cast<u8>((header >> 8) & 0xfc);
    }
    return 0;
}

bool enable_msi(const Device& d, u8 vector, u32 apic_id) {
    constexpr u8 kCapMsi = 0x05, kCapMsix = 0x11;
    // Fixed delivery, physical destination: the local APIC's id in bits 19:12.
    const u32 address = 0xfee00000u | ((apic_id & 0xff) << 12);

    if (const u8 cap = find_capability(d, kCapMsi)) {
        u16 control = read16(d, static_cast<u8>(cap + 2));
        const bool is64 = control & 0x80, maskable = control & 0x100;
        write32(d, static_cast<u8>(cap + 4), address);
        u8 data = static_cast<u8>(cap + 8);
        if (is64) { write32(d, static_cast<u8>(cap + 8), 0); data = static_cast<u8>(cap + 12); }
        write16(d, data, vector);
        if (maskable) write32(d, static_cast<u8>(data + 4), 0);    // unmask
        control = static_cast<u16>((control & ~0x70u) | 1u);        // one message, enabled
        write16(d, static_cast<u8>(cap + 2), control);
        return true;
    }
    if (const u8 cap = find_capability(d, kCapMsix)) {
        const u32 table = read32(d, static_cast<u8>(cap + 4));
        const Bar b = bar(d, table & 7);
        if (b.io || b.address == 0) return false;
        const u64 mapped = vmm::map_mmio(b.address + (table & ~7u), 16);
        if (mapped == 0) return false;
        auto* entry = reinterpret_cast<volatile u32*>(mapped);
        entry[0] = address;
        entry[1] = 0;
        entry[2] = vector;
        entry[3] = 0;                                               // unmasked
        u16 control = read16(d, static_cast<u8>(cap + 2));
        control = static_cast<u16>((control | 0x8000u) & ~0x4000u); // enabled, function not masked
        write16(d, static_cast<u8>(cap + 2), control);
        return true;
    }
    return false;
}

} // namespace pci
