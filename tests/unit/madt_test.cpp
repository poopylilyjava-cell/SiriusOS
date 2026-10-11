// Host-side tests for the ACPI MADT parser. The table comes from firmware,
// so it is untrusted input: each malformed case is run under asan, with the
// table in a heap block of exactly its size.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <acpi/madt.cpp>

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

namespace {

struct Builder {
    std::vector<uint8_t> entries;

    Builder& io_apic(uint8_t id, uint32_t address, uint32_t gsi_base) {
        const uint8_t head[4] = { 1, 12, id, 0 };
        entries.insert(entries.end(), head, head + 4);
        append32(address); append32(gsi_base);
        return *this;
    }
    Builder& override_irq(uint8_t source, uint32_t gsi, uint16_t flags) {
        const uint8_t head[4] = { 2, 10, 0, source };
        entries.insert(entries.end(), head, head + 4);
        append32(gsi);
        entries.push_back(flags & 0xff); entries.push_back(flags >> 8);
        return *this;
    }
    Builder& raw(std::initializer_list<int> bytes) {
        for (int b : bytes) entries.push_back(static_cast<uint8_t>(b));
        return *this;
    }
    void append32(uint32_t v) { for (int i = 0; i < 4; i++) entries.push_back((v >> (8 * i)) & 0xff); }

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> table(44, 0);
        memcpy(table.data(), "APIC", 4);
        table.insert(table.end(), entries.begin(), entries.end());
        const uint32_t length = static_cast<uint32_t>(table.size());
        memcpy(table.data() + 4, &length, 4);
        uint8_t sum = 0;
        for (uint8_t b : table) sum = static_cast<uint8_t>(sum + b);
        table[9] = static_cast<uint8_t>(-sum);
        return table;
    }
};

bool parse(const std::vector<uint8_t>& table, acpi::Madt& out, size_t readable) {
    auto* copy = static_cast<uint8_t*>(malloc(readable ? readable : 1));
    memcpy(copy, table.data(), readable < table.size() ? readable : table.size());
    const bool ok = acpi::parse_madt(copy, readable, out);
    free(copy);
    return ok;
}

bool parse(const std::vector<uint8_t>& table, acpi::Madt& out) { return parse(table, out, table.size()); }

bool is_default(const acpi::Madt& madt) {
    if (madt.io_apic_count != 0) return false;
    for (uint32_t irq = 0; irq < 16; irq++)
        if (madt.isa[irq].gsi != irq || madt.isa[irq].active_low || madt.isa[irq].level_triggered) return false;
    return true;
}

} // namespace

int main() {
    printf("a PC's table:\n");
    {
        // What QEMU's q35 reports: one I/O APIC, the timer on GSI 2, and the
        // ACPI interrupt level-triggered and active-high.
        acpi::Madt madt;
        const auto table = Builder().raw({ 0, 8, 0, 0, 1, 0, 0, 0 })   // a local APIC entry: ignored
                               .io_apic(0, 0xfec00000, 0)
                               .override_irq(0, 2, 0)
                               .override_irq(9, 9, 0x000d)
                               .override_irq(11, 11, 0x000f)
                               .build();
        expect(parse(table, madt), "a well-formed table parses");
        expect(madt.io_apic_count == 1 && madt.io_apics[0].address == 0xfec00000 && madt.io_apics[0].gsi_base == 0,
               "the I/O APIC is found");
        expect(madt.isa[0].gsi == 2 && !madt.isa[0].active_low && !madt.isa[0].level_triggered,
               "an override moves an IRQ to another GSI, keeping the bus defaults");
        expect(madt.isa[9].gsi == 9 && madt.isa[9].level_triggered && !madt.isa[9].active_low,
               "level-triggered, active-high is decoded");
        expect(madt.isa[11].level_triggered && madt.isa[11].active_low, "and level-triggered, active-low");
        expect(madt.isa[1].gsi == 1 && madt.isa[12].gsi == 12 && !madt.isa[1].level_triggered,
               "IRQs without an override keep their own number");
    }
    {
        acpi::Madt madt;
        Builder b;
        for (int i = 0; i < 6; i++) b.io_apic(static_cast<uint8_t>(i), 0xfec00000 + i * 0x1000, i * 24);
        expect(parse(b.build(), madt) && madt.io_apic_count == acpi::Madt::kMaxIoApics, "more I/O APICs than fit are ignored, not overrun");
        expect(parse(Builder().override_irq(200, 5, 0).raw({ 0x7f, 2 }).build(), madt) && is_default(madt),
               "an override for an IRQ that does not exist, and unknown entry types, are skipped");
    }

    printf("malformed tables:\n");
    {
        acpi::Madt madt;
        const auto good = Builder().io_apic(0, 0xfec00000, 0).build();

        expect(!parse(good, madt, 20) && is_default(madt), "a table shorter than its header is rejected");
        expect(!parse(good, madt, good.size() - 1) && is_default(madt), "a table longer than what is readable is rejected");

        auto bad = good; memcpy(bad.data(), "FACP", 4);
        expect(!parse(bad, madt) && is_default(madt), "another table's signature is rejected");

        bad = good; bad[50] ^= 0x55;
        expect(!parse(bad, madt) && is_default(madt), "a wrong checksum is rejected");

        // The entries themselves lie about their length.
        expect(!parse(Builder().raw({ 1, 0 }).build(), madt) && is_default(madt), "a zero-length entry is rejected, not looped on");
        expect(!parse(Builder().io_apic(0, 0xfec00000, 0).raw({ 1, 12, 0, 0 }).build(), madt) && is_default(madt),
               "an entry running past the end of the table is rejected, and nothing before it is kept");
        expect(parse(Builder().raw({ 1, 4, 0, 0 }).build(), madt) && is_default(madt),
               "an I/O APIC entry too short to hold an address is ignored");

        expect(!acpi::parse_madt(nullptr, 0, madt) && is_default(madt), "no table at all leaves the defaults");
    }

    printf("checksums:\n");
    {
        const uint8_t zero_sum[4] = { 1, 2, 3, 250 };
        const uint8_t other[4] = { 1, 2, 3, 251 };
        expect(acpi::checksum_ok(zero_sum, 4) && !acpi::checksum_ok(other, 4), "bytes must sum to zero");
    }

    if (g_failures) {
        printf("madt_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("madt_test: all passed\n");
    return 0;
}
