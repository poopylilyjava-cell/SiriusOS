#include <acpi/fadt.hpp>
#include <klib/string.hpp>

namespace acpi {
namespace {

u32 read32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
u64 read64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }

// A Generic Address Structure at `p`: space, bit width, bit offset, access
// size, then a 64-bit address.
Register gas(const u8* p) {
    Register r;
    if (p[0] != 0 && p[0] != 1) return r;   // only memory and I/O space are of use here
    r.space = static_cast<Register::Space>(p[0]);
    r.bit_width = p[1];
    r.address = read64(p + 4);
    if (r.address == 0) r.space = Register::Space::None;
    return r;
}

// An I/O port block from the old 32-bit fields.
Register io_block(u32 port, u8 width) {
    Register r;
    if (port == 0) return r;
    r.space = Register::Space::Io;
    r.bit_width = width;
    r.address = port;
    return r;
}

// Offsets of the fields read, in the FADT of ACPI 6.
constexpr usize kDsdt = 40, kSmiCommand = 48, kAcpiEnable = 52, kPm1aControl = 64, kPm1bControl = 68,
                kPmTimer = 76, kPm1ControlLength = 89, kPmTimerLength = 91, kFlags = 112, kReset = 116,
                kResetValue = 128, kXDsdt = 140, kXPm1aControl = 172, kXPm1bControl = 184, kXPmTimer = 208,
                kSleepControl = 244;
constexpr u32 kFlagTimer32 = 1u << 8, kFlagResetSupported = 1u << 10, kFlagHardwareReduced = 1u << 20;

} // namespace

bool parse_fadt(const void* table, usize length, Fadt& out) {
    out = Fadt();
    const auto* t = static_cast<const u8*>(table);
    // ACPI 1.0's FADT ended at 116; everything after is optional, and only
    // read if the table is long enough to hold it.
    if (table == nullptr || length < 116 || memcmp(t, "FACP", 4) != 0) return false;
    auto has = [&](usize offset, usize size) { return offset + size <= length; };

    const u32 flags = read32(t + kFlags);
    out.dsdt = read32(t + kDsdt);
    out.smi_command = read32(t + kSmiCommand);
    out.acpi_enable = t[kAcpiEnable];
    out.pm_timer_32bit = flags & kFlagTimer32;
    out.pm_timer = io_block(read32(t + kPmTimer), t[kPmTimerLength] == 4 ? 32 : 0);
    out.pm1a_control = io_block(read32(t + kPm1aControl), static_cast<u8>(t[kPm1ControlLength] * 8));
    out.pm1b_control = io_block(read32(t + kPm1bControl), static_cast<u8>(t[kPm1ControlLength] * 8));
    out.hardware_reduced = flags & kFlagHardwareReduced;

    if ((flags & kFlagResetSupported) && has(kResetValue, 1)) {
        out.reset = gas(t + kReset);
        out.reset_value = t[kResetValue];
    }
    // The 64-bit fields, where present, take precedence over the old ones.
    if (has(kXDsdt, 8) && read64(t + kXDsdt) != 0) out.dsdt = read64(t + kXDsdt);
    if (has(kXPm1aControl, 12) && gas(t + kXPm1aControl).present()) out.pm1a_control = gas(t + kXPm1aControl);
    if (has(kXPm1bControl, 12) && gas(t + kXPm1bControl).present()) out.pm1b_control = gas(t + kXPm1bControl);
    if (has(kXPmTimer, 12) && gas(t + kXPmTimer).present()) out.pm_timer = gas(t + kXPmTimer);
    if (has(kSleepControl, 12)) out.sleep_control = gas(t + kSleepControl);
    return true;
}

u64 parse_hpet(const void* table, usize length) {
    const auto* t = static_cast<const u8*>(table);
    if (table == nullptr || length < 56 || memcmp(t, "HPET", 4) != 0) return 0;
    const Register r = gas(t + 40);
    return r.space == Register::Space::Memory ? r.address : 0;
}

bool find_s5(const void* aml, usize length, u8& slp_typ_a, u8& slp_typ_b) {
    const auto* a = static_cast<const u8*>(aml);
    if (aml == nullptr) return false;
    // Name (_S5, Package () { a, b, ... }): "_S5_", PackageOp, PkgLength,
    // NumElements, then the values -- each a byte constant (0x0A n), or
    // Zero, One or Ones.
    for (usize i = 0; i + 8 < length; i++) {
        if (memcmp(a + i, "_S5_", 4) != 0) continue;
        // A reference to _S5 elsewhere is not its definition: that is
        // preceded by NameOp, possibly with a root or parent prefix.
        if (i == 0 || !(a[i - 1] == 0x08 || (i >= 2 && a[i - 2] == 0x08 && (a[i - 1] == '\\' || a[i - 1] == '^'))))
            continue;
        usize p = i + 4;
        if (a[p++] != 0x12) continue;                       // PackageOp
        p += static_cast<usize>((a[p] >> 6) & 3) + 1;       // PkgLength: its first byte says how many follow
        if (p >= length) return false;
        p++;                                                // NumElements
        u8 values[2];
        for (u8& value : values) {
            if (p >= length) return false;
            const u8 op = a[p++];
            if (op == 0x0a) { if (p >= length) return false; value = a[p++]; }   // BytePrefix
            else if (op == 0x00 || op == 0x01) value = op;                       // Zero, One
            else if (op == 0xff) value = 0xff;                                   // Ones
            else return false;
        }
        slp_typ_a = values[0];
        slp_typ_b = values[1];
        return true;
    }
    return false;
}

} // namespace acpi
