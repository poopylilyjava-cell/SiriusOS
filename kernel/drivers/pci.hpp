#pragma once
#include <klib/types.hpp>

// PCI configuration space, through the legacy 0xcf8/0xcfc ports. That reaches
// the first 256 bytes of every function, which is all M3's drivers need;
// PCIe extended space (ECAM, via the ACPI MCFG table) comes with ACPI.
namespace pci {

struct Device {
    u8  bus, slot, function;
    u16 vendor_id, device_id;
    u8  class_code, subclass, prog_if;
};

struct Bar {
    u64  address;  // physical address, or I/O port base
    u64  size;
    bool io;
    bool prefetchable;
};

// Enumerate every bus and log what is found.
void init();

u32 count();
const Device& at(u32 index);

// The `index`th device of the given class, or null.
const Device* find_class(u8 class_code, u8 subclass, u8 prog_if, u32 index = 0);

u32  read32(const Device& device, u8 offset);
u16  read16(const Device& device, u8 offset);
void write32(const Device& device, u8 offset, u32 value);
void write16(const Device& device, u8 offset, u16 value);

// Decode base address register `index` (0-5), sizing it. A 64-bit BAR spans
// two slots; ask for the lower one.
Bar bar(const Device& device, u32 index);

// Let the device decode memory accesses and master the bus (DMA). Legacy INTx
// is disabled: drivers poll, or use MSI.
void enable_dma(const Device& device);

// The offset of the device's capability `id` in its configuration space, or
// 0 if it has none.
u8 find_capability(const Device& device, u8 id);

// Have the device interrupt by message: `vector` on the CPU whose local APIC
// id is `apic_id`. MSI if the device has it, otherwise the first entry of
// its MSI-X table. False if it has neither. (Legacy INTx would need the
// firmware's AML to know how the device is wired; messages need nothing.)
bool enable_msi(const Device& device, u8 vector, u32 apic_id);

} // namespace pci
