#include <acpi/acpi.hpp>
#include <boot/requests.hpp>
#include <mm/memory.hpp>
#include <mm/paging_math.hpp>
#include <mm/vmm.hpp>
#include <arch/x86_64/cpu.hpp>
#include <klib/spinlock.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>

namespace acpi {
namespace {

constexpr u32 kDefaultIoApicAddress = 0xfec00000;

struct [[gnu::packed]] Rsdp {
    char signature[8];   // "RSD PTR "
    u8   checksum;       // of the first 20 bytes
    char oem_id[6];
    u8   revision;       // 0: ACPI 1.0, with only the RSDT; 2: the XSDT too
    u32  rsdt_address;
    u32  length;
    u64  xsdt_address;
    u8   extended_checksum;
    u8   reserved[3];
};

Madt g_madt;
Fadt g_fadt;
u64 g_hpet = 0;
bool g_have_s5 = false;
u8 g_slp_typ_a = 0, g_slp_typ_b = 0;

// Firmware tables sit in memory the direct map covers when the memory map
// calls it ACPI data, and does not when it calls it reserved. Use the direct
// map where it reaches and a fresh mapping where it does not.
const void* map_physical(u64 physical, u64 length) {
    const u64 hhdm_address = reinterpret_cast<u64>(mm::phys_to_virt<void>(physical));
    bool covered = true;
    for (u64 page = mm::align_down(hhdm_address, mm::kPageSize); page < hhdm_address + length; page += mm::kPageSize)
        if (vmm::kernel_space().translate(page) == 0) { covered = false; break; }
    if (covered) return reinterpret_cast<const void*>(hhdm_address);
    return reinterpret_cast<const void*>(vmm::map_mmio(physical, length));
}

// A whole table: its header says how long it is.
const TableHeader* map_table(u64 physical) {
    const auto* header = static_cast<const TableHeader*>(map_physical(physical, sizeof(TableHeader)));
    if (header == nullptr || header->length < sizeof(TableHeader) || header->length > 1024 * 1024) return nullptr;
    return static_cast<const TableHeader*>(map_physical(physical, header->length));
}

const TableHeader* find_table(const Rsdp* rsdp, const char* signature) {
    const bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const TableHeader* root = map_table(use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address);
    if (root == nullptr || !checksum_ok(root, root->length)) return nullptr;

    // The root table is an array of pointers to the others: 64-bit ones in
    // the XSDT, 32-bit in the RSDT. They are not aligned.
    const usize entry_size = use_xsdt ? 8 : 4;
    const auto* entries = reinterpret_cast<const u8*>(root) + sizeof(TableHeader);
    for (usize i = 0; i < (root->length - sizeof(TableHeader)) / entry_size; i++) {
        u64 address = 0;
        memcpy(&address, entries + i * entry_size, entry_size);
        const TableHeader* table = map_table(address);
        if (table && memcmp(table->signature, signature, 4) == 0) return table;
    }
    return nullptr;
}

} // namespace

void init() {
    parse_madt(nullptr, 0, g_madt);   // the defaults

    const TableHeader* table = nullptr;
    const Rsdp* rsdp = nullptr;
    if (g_rsdp_request.response != nullptr && g_rsdp_request.response->address != nullptr) {
        // Whether this is a physical address or one in the direct map has
        // changed between revisions of the boot protocol. The two cannot be
        // confused: the direct map is in the higher half.
        u64 physical = reinterpret_cast<u64>(g_rsdp_request.response->address);
        if (physical >= mm::hhdm_offset()) physical -= mm::hhdm_offset();
        rsdp = static_cast<const Rsdp*>(map_physical(physical, sizeof(Rsdp)));
        if (rsdp != nullptr && memcmp(rsdp->signature, "RSD PTR ", 8) == 0 && checksum_ok(rsdp, 20))
            table = find_table(rsdp, "APIC");
        else
            rsdp = nullptr;
    }

    if (rsdp != nullptr) {
        const TableHeader* facp = find_table(rsdp, "FACP");
        if (facp != nullptr && parse_fadt(facp, facp->length, g_fadt)) {
            LOG_INFO("acpi: pm timer %s %#lx (%u bits), reset register %s, %s",
                     g_fadt.pm_timer.space == Register::Space::Io ? "port" : "at", g_fadt.pm_timer.address,
                     g_fadt.pm_timer_32bit ? 32u : 24u, g_fadt.reset.present() ? "present" : "absent",
                     g_fadt.hardware_reduced ? "hardware-reduced" : "full ACPI");
            if (g_fadt.dsdt != 0) {
                const TableHeader* dsdt = map_table(g_fadt.dsdt);
                if (dsdt != nullptr && memcmp(dsdt->signature, "DSDT", 4) == 0)
                    g_have_s5 = find_s5(reinterpret_cast<const u8*>(dsdt) + sizeof(TableHeader),
                                        dsdt->length - sizeof(TableHeader), g_slp_typ_a, g_slp_typ_b);
            }
            if (g_have_s5) LOG_INFO("acpi: soft off is sleep type %u/%u", g_slp_typ_a, g_slp_typ_b);
            else LOG_WARN("acpi: no \\_S5 in the DSDT: the machine cannot switch itself off");
        }
        const TableHeader* hpet = find_table(rsdp, "HPET");
        if (hpet != nullptr) g_hpet = parse_hpet(hpet, hpet->length);
        if (g_hpet) LOG_INFO("acpi: hpet at %#lx", g_hpet);
    }

    if (table == nullptr || !parse_madt(table, table->length, g_madt))
        LOG_WARN("acpi: no usable MADT; assuming the PC's default interrupt wiring");
    if (g_madt.io_apic_count == 0) {
        g_madt.io_apics[0] = { 0, kDefaultIoApicAddress, 0 };
        g_madt.io_apic_count = 1;
    }

    for (u32 i = 0; i < g_madt.io_apic_count; i++)
        LOG_INFO("acpi: i/o apic %u at %#x, from gsi %u", g_madt.io_apics[i].id, g_madt.io_apics[i].address,
                 g_madt.io_apics[i].gsi_base);
    for (u32 irq = 0; irq < 16; irq++) {
        const IsaRoute& route = g_madt.isa[irq];
        if (route.gsi != irq || route.active_low || route.level_triggered)
            LOG_DEBUG("acpi: irq %u -> gsi %u, %s, %s", irq, route.gsi, route.active_low ? "active low" : "active high",
                      route.level_triggered ? "level" : "edge");
    }
}

const Madt& madt() { return g_madt; }
const Fadt& fadt() { return g_fadt; }
u64 hpet_address() { return g_hpet; }

namespace {

u32 read_register(const Register& r) {
    if (!r.present()) return 0;
    if (r.space == Register::Space::Io) {
        const u16 port = static_cast<u16>(r.address);
        return r.bit_width == 8 ? cpu::inb(port) : r.bit_width == 32 ? cpu::inl(port) : cpu::inw(port);
    }
    const auto* p = reinterpret_cast<const volatile u8*>(map_physical(r.address, 4));
    return r.bit_width == 8 ? *p : r.bit_width == 32 ? *reinterpret_cast<const volatile u32*>(p) : *reinterpret_cast<const volatile u16*>(p);
}

void write_register(const Register& r, u32 value) {
    if (!r.present()) return;
    if (r.space == Register::Space::Io) {
        const u16 port = static_cast<u16>(r.address);
        if (r.bit_width == 8) cpu::outb(port, static_cast<u8>(value));
        else if (r.bit_width == 32) cpu::outl(port, value);
        else cpu::outw(port, static_cast<u16>(value));
        return;
    }
    auto* p = reinterpret_cast<volatile u8*>(const_cast<void*>(map_physical(r.address, 4)));
    if (r.bit_width == 8) *p = static_cast<u8>(value);
    else if (r.bit_width == 32) *reinterpret_cast<volatile u32*>(p) = value;
    else *reinterpret_cast<volatile u16*>(p) = static_cast<u16>(value);
}

constexpr u32 kSciEnable = 1u << 0, kSleepEnable = 1u << 13;

} // namespace

void power_off() {
    if (!g_have_s5) { LOG_ERROR("acpi: cannot power off: no \\_S5"); return; }
    LOG_INFO("acpi: powering off");
    irq::save();
    if (g_fadt.hardware_reduced && g_fadt.sleep_control.present()) {
        write_register(g_fadt.sleep_control, (static_cast<u32>(g_slp_typ_a & 7) << 2) | (1u << 5));
    } else if (g_fadt.pm1a_control.present()) {
        // Firmware that booted in legacy mode is asked to hand over first.
        if (!(read_register(g_fadt.pm1a_control) & kSciEnable) && g_fadt.smi_command && g_fadt.acpi_enable) {
            cpu::outb(static_cast<u16>(g_fadt.smi_command), g_fadt.acpi_enable);
            for (u32 i = 0; i < 1'000'000 && !(read_register(g_fadt.pm1a_control) & kSciEnable); i++) cpu::io_wait();
        }
        const u32 a = read_register(g_fadt.pm1a_control) & ~(7u << 10);
        write_register(g_fadt.pm1a_control, a | (static_cast<u32>(g_slp_typ_a & 7) << 10) | kSleepEnable);
        if (g_fadt.pm1b_control.present()) {
            const u32 b = read_register(g_fadt.pm1b_control) & ~(7u << 10);
            write_register(g_fadt.pm1b_control, b | (static_cast<u32>(g_slp_typ_b & 7) << 10) | kSleepEnable);
        }
    }
    // Still here: give the hardware a moment, then admit defeat.
    for (u32 i = 0; i < 1'000'000; i++) cpu::io_wait();
    LOG_ERROR("acpi: the machine did not switch off");
}

void reboot() {
    LOG_INFO("acpi: restarting");
    irq::save();
    if (g_fadt.reset.present()) {
        write_register(g_fadt.reset, g_fadt.reset_value);
        for (u32 i = 0; i < 100'000; i++) cpu::io_wait();
    }
    // The keyboard controller's reset line, then the PCI reset control
    // register, on machines that have them.
    for (u32 i = 0; i < 100'000 && (cpu::inb(0x64) & 2); i++) cpu::io_wait();
    cpu::outb(0x64, 0xfe);
    for (u32 i = 0; i < 100'000; i++) cpu::io_wait();
    cpu::outb(0xcf9, 0x06);
    for (u32 i = 0; i < 100'000; i++) cpu::io_wait();
    // Last resort: an interrupt with no descriptor table is a triple fault.
    struct [[gnu::packed]] { u16 limit; u64 base; } empty{ 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(empty));
    cpu::halt_forever();
}

} // namespace acpi
