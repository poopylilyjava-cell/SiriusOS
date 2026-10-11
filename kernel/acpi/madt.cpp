#include <acpi/madt.hpp>

namespace acpi {
namespace {

constexpr u8 kEntryIoApic = 1;
constexpr u8 kEntryInterruptOverride = 2;

// After the common header: the local APIC address and flags, then entries.
constexpr usize kEntriesOffset = sizeof(TableHeader) + 8;

u16 read16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 read32(const u8* p) { return static_cast<u32>(read16(p)) | (static_cast<u32>(read16(p + 2)) << 16); }

void set_defaults(Madt& out) {
    out = {};
    for (u32 irq = 0; irq < 16; irq++) out.isa[irq] = { irq, false, false };
}

} // namespace

bool checksum_ok(const void* data, usize length) {
    const auto* bytes = static_cast<const u8*>(data);
    u8 sum = 0;
    for (usize i = 0; i < length; i++) sum = static_cast<u8>(sum + bytes[i]);
    return sum == 0;
}

bool parse_madt(const void* table, usize size, Madt& out) {
    set_defaults(out);
    const auto* bytes = static_cast<const u8*>(table);
    if (size < kEntriesOffset) return false;
    if (bytes[0] != 'A' || bytes[1] != 'P' || bytes[2] != 'I' || bytes[3] != 'C') return false;
    const u32 length = read32(bytes + 4);
    if (length < kEntriesOffset || length > size) return false;
    if (!checksum_ok(bytes, length)) return false;

    // Each entry is a type byte, a length byte, and that many bytes in all.
    for (usize offset = kEntriesOffset; offset + 2 <= length;) {
        const u8 type = bytes[offset], entry_length = bytes[offset + 1];
        if (entry_length < 2 || offset + entry_length > length) { set_defaults(out); return false; }
        const u8* entry = bytes + offset;

        if (type == kEntryIoApic && entry_length >= 12) {
            if (out.io_apic_count < Madt::kMaxIoApics)
                out.io_apics[out.io_apic_count++] = { entry[2], read32(entry + 4), read32(entry + 8) };
        } else if (type == kEntryInterruptOverride && entry_length >= 10) {
            // bus (always 0, ISA), source IRQ, GSI, then MPS INTI flags:
            // bits 0-1 polarity and bits 2-3 trigger mode, where 0 means
            // "as the bus defines" -- for ISA, active high and edge -- and
            // 3 means the opposite.
            const u8 source = entry[3];
            const u16 flags = read16(entry + 8);
            if (source < 16)
                out.isa[source] = { read32(entry + 4), (flags & 0x3) == 0x3, ((flags >> 2) & 0x3) == 0x3 };
        }
        offset += entry_length;
    }
    return true;
}

} // namespace acpi
