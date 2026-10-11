#pragma once
#include <klib/types.hpp>

// The ACPI tables the kernel reads, as pure parsing over bytes in memory:
// finding and mapping the tables is acpi.cpp's job, and everything here runs
// in the host tests against hand-built (and deliberately broken) tables.
namespace acpi {

// Every table starts with this.
struct [[gnu::packed]] TableHeader {
    char signature[4];
    u32  length;         // of the whole table, header included
    u8   revision;
    u8   checksum;       // all `length` bytes sum to zero
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
};
static_assert(sizeof(TableHeader) == 36, "ACPI table headers are 36 bytes");

// True if `length` bytes at `data` sum to zero modulo 256.
bool checksum_ok(const void* data, usize length);

// How one legacy ISA interrupt reaches an I/O APIC.
struct IsaRoute {
    u32  gsi;            // global system interrupt it is wired to
    bool active_low;
    bool level_triggered;
};

struct IoApic {
    u8  id;
    u32 address;         // physical address of its registers
    u32 gsi_base;        // the first global system interrupt it serves
};

// What the Multiple APIC Description Table says about interrupt routing.
struct Madt {
    static constexpr u32 kMaxIoApics = 4;
    IoApic io_apics[kMaxIoApics];
    u32    io_apic_count;
    // Indexed by ISA IRQ. Without an override an IRQ is wired to the GSI of
    // the same number, edge triggered and active high.
    IsaRoute isa[16];
};

// Parse a MADT (signature "APIC"). `size` is how many bytes are readable at
// `table`. False if the table is malformed; `out` is then the defaults.
bool parse_madt(const void* table, usize size, Madt& out);

} // namespace acpi
