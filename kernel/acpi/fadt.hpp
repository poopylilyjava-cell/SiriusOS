#pragma once
#include <acpi/madt.hpp>

// What the kernel reads from the FADT, the HPET table and the DSDT: timers
// to measure time with where a PC's PIT is missing, and the registers that
// switch the machine off and restart it. Pure parsing over bytes, like
// madt.cpp, and tested on the host against built (and broken) tables.
namespace acpi {

// A register described by a Generic Address Structure.
struct Register {
    enum class Space : u8 { Memory = 0, Io = 1, None = 0xff };
    Space space = Space::None;
    u8    bit_width = 0;
    u64   address = 0;
    bool present() const { return space != Space::None && address != 0; }
};

struct Fadt {
    Register pm_timer;          // the ACPI power management timer, 3.579545 MHz
    bool     pm_timer_32bit = false;
    Register pm1a_control, pm1b_control;
    Register reset;             // write reset_value here to restart
    u8       reset_value = 0;
    u32      smi_command = 0;   // where to write acpi_enable, if the firmware is not in ACPI mode
    u8       acpi_enable = 0;
    bool     hardware_reduced = false;
    Register sleep_control;     // hardware-reduced ACPI's way to sleep
    u64      dsdt = 0;          // physical address
};

constexpr u64 kPmTimerHz = 3579545;

// Parse a FADT ("FACP"). False if it is too short to be one.
bool parse_fadt(const void* table, usize length, Fadt& out);

// The HPET table: the physical address of the timer's registers, or 0.
u64 parse_hpet(const void* table, usize length);

// The \_S5 (soft off) sleep type values, found by scanning AML for the
// package that names them -- not by interpreting it. False if not found.
bool find_s5(const void* aml, usize length, u8& slp_typ_a, u8& slp_typ_b);

} // namespace acpi
