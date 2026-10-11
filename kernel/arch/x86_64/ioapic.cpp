#include <arch/x86_64/ioapic.hpp>
#include <acpi/acpi.hpp>
#include <mm/vmm.hpp>
#include <klib/print.hpp>
#include <klib/spinlock.hpp>

namespace ioapic {
namespace {

// The chip has two memory-mapped registers: write an index to one, then
// read or write that register through the other.
constexpr u32 kRegisterSelect = 0x00;
constexpr u32 kRegisterWindow = 0x10;

constexpr u32 kIndexVersion     = 0x01;   // bits 16-23: the highest redirection entry
constexpr u32 kIndexRedirection = 0x10;   // two 32-bit registers per input

constexpr u32 kRedirectionActiveLow = 1u << 13;
constexpr u32 kRedirectionLevel     = 1u << 15;
constexpr u32 kRedirectionMasked    = 1u << 16;

struct Chip {
    volatile u32* mmio;
    u32 gsi_base;
    u32 inputs;
};

Chip g_chips[acpi::Madt::kMaxIoApics];
u32 g_chip_count = 0;
Spinlock g_lock;   // the select/window pair is not atomic

u32 read(const Chip& chip, u32 index) {
    chip.mmio[kRegisterSelect / 4] = index;
    return chip.mmio[kRegisterWindow / 4];
}

void write(const Chip& chip, u32 index, u32 value) {
    chip.mmio[kRegisterSelect / 4] = index;
    chip.mmio[kRegisterWindow / 4] = value;
}

} // namespace

void init() {
    const acpi::Madt& madt = acpi::madt();
    for (u32 i = 0; i < madt.io_apic_count; i++) {
        const u64 address = vmm::map_mmio(madt.io_apics[i].address, 0x20);
        if (address == 0) { LOG_ERROR("ioapic: cannot map the registers at %#x", madt.io_apics[i].address); continue; }

        Chip& chip = g_chips[g_chip_count++];
        chip.mmio = reinterpret_cast<volatile u32*>(address);
        chip.gsi_base = madt.io_apics[i].gsi_base;
        chip.inputs = ((read(chip, kIndexVersion) >> 16) & 0xff) + 1;
        // The firmware may have left inputs routed for its own use.
        for (u32 input = 0; input < chip.inputs; input++)
            write(chip, kIndexRedirection + input * 2, kRedirectionMasked);
        LOG_INFO("ioapic: %u inputs from gsi %u, all masked", chip.inputs, chip.gsi_base);
    }
}

bool route_isa_irq(u8 irq, u8 vector, u32 apic_id) {
    if (irq >= 16) return false;
    const acpi::IsaRoute& route = acpi::madt().isa[irq];

    ScopedLock guard(g_lock);
    for (u32 i = 0; i < g_chip_count; i++) {
        const Chip& chip = g_chips[i];
        if (route.gsi < chip.gsi_base || route.gsi >= chip.gsi_base + chip.inputs) continue;

        // Fixed delivery to one CPU, by its physical APIC id. The high half
        // goes first: the entry is live as soon as the low half is unmasked.
        const u32 index = kIndexRedirection + (route.gsi - chip.gsi_base) * 2;
        u32 low = vector;
        if (route.active_low)      low |= kRedirectionActiveLow;
        if (route.level_triggered) low |= kRedirectionLevel;
        write(chip, index + 1, apic_id << 24);
        write(chip, index, low);
        LOG_DEBUG("ioapic: irq %u (gsi %u) -> vector %#x on apic %u", irq, route.gsi, vector, apic_id);
        return true;
    }
    LOG_ERROR("ioapic: no i/o apic serves irq %u (gsi %u)", irq, route.gsi);
    return false;
}

} // namespace ioapic
