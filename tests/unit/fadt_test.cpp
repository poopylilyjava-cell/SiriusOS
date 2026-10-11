// Host-side tests for the FADT, HPET and \_S5 parsers: firmware tables are
// untrusted input, so each is run under asan in a block of exactly its size.

#include <cstdio>
#include <cstring>
#include <vector>

#include <acpi/fadt.cpp>

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

namespace {

void put32(std::vector<uint8_t>& t, size_t at, uint32_t v) { memcpy(&t[at], &v, 4); }
void put64(std::vector<uint8_t>& t, size_t at, uint64_t v) { memcpy(&t[at], &v, 8); }
void gas(std::vector<uint8_t>& t, size_t at, uint8_t space, uint8_t width, uint64_t address) {
    t[at] = space; t[at + 1] = width; t[at + 2] = 0; t[at + 3] = 0;
    put64(t, at + 4, address);
}

std::vector<uint8_t> table(const char* signature, size_t length) {
    std::vector<uint8_t> t(length, 0);
    memcpy(t.data(), signature, 4);
    put32(t, 4, (uint32_t)length);
    return t;
}

} // namespace

int main() {
    printf("fadt:\n");
    {
        // ACPI 1.0: 116 bytes, I/O ports only.
        auto t = table("FACP", 116);
        put32(t, 40, 0x7fe00000);       // DSDT
        put32(t, 64, 0x404);            // PM1a control
        put32(t, 76, 0x408);            // PM timer
        t[89] = 2; t[91] = 4;
        put32(t, 112, 1u << 8);         // 32-bit timer
        acpi::Fadt f;
        expect(acpi::parse_fadt(t.data(), t.size(), f), "an ACPI 1.0 FADT parses");
        expect(f.pm_timer.present() && f.pm_timer.space == acpi::Register::Space::Io && f.pm_timer.address == 0x408,
               "its PM timer is the I/O port block");
        expect(f.pm_timer_32bit, "and is 32 bits wide, as the flags say");
        expect(f.pm1a_control.address == 0x404 && f.pm1a_control.bit_width == 16 && !f.pm1b_control.present(),
               "PM1a control is there, PM1b is not");
        expect(f.dsdt == 0x7fe00000 && !f.reset.present(), "the DSDT; no reset register without the flag");
    }
    {
        // ACPI 6: the 64-bit fields win; reset register; hardware reduced.
        auto t = table("FACP", 276);
        put32(t, 76, 0x408);
        put32(t, 112, (1u << 10) | (1u << 20));
        gas(t, 116, 1, 8, 0xcf9);
        t[128] = 0x06;
        put64(t, 140, 0x1'2345'6000ULL);
        gas(t, 208, 0, 32, 0xfed80000);  // X_PM_TMR_BLK in memory space
        gas(t, 244, 1, 8, 0x800);
        acpi::Fadt f;
        expect(acpi::parse_fadt(t.data(), t.size(), f), "an ACPI 6 FADT parses");
        expect(f.pm_timer.space == acpi::Register::Space::Memory && f.pm_timer.address == 0xfed80000,
               "the extended PM timer field replaces the old one");
        expect(f.reset.present() && f.reset.address == 0xcf9 && f.reset_value == 6, "the reset register and value");
        expect(f.dsdt == 0x1'2345'6000ULL, "the 64-bit DSDT address");
        expect(f.hardware_reduced && f.sleep_control.address == 0x800, "hardware-reduced, with a sleep control register");
    }
    {
        auto t = table("FACP", 100);
        acpi::Fadt f;
        expect(!acpi::parse_fadt(t.data(), t.size(), f), "a truncated FADT is refused");
        auto u = table("APIC", 200);
        expect(!acpi::parse_fadt(u.data(), u.size(), f), "so is another table");
    }

    printf("hpet:\n");
    {
        auto t = table("HPET", 56);
        gas(t, 40, 0, 64, 0xfed00000);
        expect(acpi::parse_hpet(t.data(), t.size()) == 0xfed00000, "the HPET's register block");
        expect(acpi::parse_hpet(t.data(), 50) == 0, "but not from a truncated table");
    }

    printf("_S5:\n");
    {
        // Name (_S5, Package (0x04) { 0x05, Zero, Zero, Zero })
        const uint8_t aml[] = { 0x10, 0x20, '\\', 0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0a, 0x05, 0x00, 0x00, 0x00, 0, 0 };
        uint8_t a = 0xee, b = 0xee;
        expect(acpi::find_s5(aml, sizeof(aml), a, b) && a == 5 && b == 0, "BytePrefix and Zero values");
    }
    {
        // Name (\_S5, Package (0x02) { 0x07, 0x07 }), after a reference to _S5_ that is not its definition.
        const uint8_t aml[] = { 'x', '_', 'S', '5', '_', 0x12, 0x08, 0x02, 0x0a, 0x01, 0x0a, 0x01,
                                0x08, '\\', '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0a, 0x07, 0x0a, 0x07, 0 };
        uint8_t a = 0, b = 0;
        expect(acpi::find_s5(aml, sizeof(aml), a, b) && a == 7 && b == 7, "the definition, not a mention");
    }
    {
        const uint8_t aml[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x40 };   // cut off inside the package
        uint8_t a, b;
        expect(!acpi::find_s5(aml, sizeof(aml), a, b), "a truncated package is not read past its end");
        const uint8_t none[] = { 0x08, '_', 'S', '3', '_', 0x12, 0x06, 0x02, 0x0a, 0x03, 0x0a, 0x03, 0 };
        expect(!acpi::find_s5(none, sizeof(none), a, b), "no _S5, no answer");
    }

    if (g_failures) {
        printf("fadt_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("fadt_test: all passed\n");
    return 0;
}
