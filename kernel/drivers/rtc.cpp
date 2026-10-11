#include <drivers/rtc.hpp>

namespace rtc {

u8 decode(u8 raw, bool bcd) {
    return bcd ? static_cast<u8>((raw >> 4) * 10 + (raw & 0x0f)) : raw;
}

u64 to_epoch(const DateTime& time) {
    // Days before each month in a common year.
    static const u16 kDaysBefore[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (time.year < 1970 || time.month < 1 || time.month > 12 || time.day < 1) return 0;

    const u64 year = time.year;
    // Leap days in the years before this one, counted from year 1 and then
    // rebased on 1970 (which has 477 behind it).
    const u64 leaps = (year - 1) / 4 - (year - 1) / 100 + (year - 1) / 400 - 477;
    u64 days = (year - 1970) * 365 + leaps + kDaysBefore[time.month - 1] + (time.day - 1);
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (leap && time.month > 2) days++;
    return days * 86400 + static_cast<u64>(time.hour) * 3600 + static_cast<u64>(time.minute) * 60 + time.second;
}

} // namespace rtc

#ifndef SIRIUS_HOST_TEST

#include <arch/x86_64/clock.hpp>
#include <arch/x86_64/cpu.hpp>
#include <klib/print.hpp>
#include <klib/spinlock.hpp>

namespace rtc {
namespace {

constexpr u16 kIndexPort = 0x70, kDataPort = 0x71;
constexpr u8 kSeconds = 0x00, kMinutes = 0x02, kHours = 0x04, kDay = 0x07, kMonth = 0x08, kYear = 0x09;
constexpr u8 kStatusA = 0x0a, kStatusB = 0x0b;
constexpr u8 kUpdateInProgress = 0x80;   // status A: the registers are changing
constexpr u8 kBinary = 0x04;             // status B: values are binary, not BCD
constexpr u8 kTwentyFourHour = 0x02;     // status B

u8 read(u8 index) {
    cpu::outb(kIndexPort, index);   // bit 7 clear: NMIs stay enabled
    return cpu::inb(kDataPort);
}

struct Raw { u8 second, minute, hour, day, month, year; bool operator==(const Raw&) const = default; };

Raw read_all() {
    for (int i = 0; i < 100'000 && (read(kStatusA) & kUpdateInProgress); i++) cpu_relax();
    return { read(kSeconds), read(kMinutes), read(kHours), read(kDay), read(kMonth), read(kYear) };
}

} // namespace

void init() {
    // The registers roll over one after another once a second. Two identical
    // readings in a row cannot both have straddled that.
    Raw raw = read_all();
    for (int attempt = 0; attempt < 10; attempt++) {
        const Raw again = read_all();
        if (again == raw) break;
        raw = again;
    }

    const u8 status = read(kStatusB);
    const bool bcd = !(status & kBinary);
    u8 hour = decode(raw.hour & 0x7f, bcd);
    if (!(status & kTwentyFourHour)) {
        // Twelve-hour mode: bit 7 of the raw hour is PM, and 12 means 0.
        hour = static_cast<u8>(hour % 12 + ((raw.hour & 0x80) ? 12 : 0));
    }
    const DateTime time{ static_cast<u16>(2000 + decode(raw.year, bcd)), decode(raw.month, bcd), decode(raw.day, bcd),
                         hour, decode(raw.minute, bcd), decode(raw.second, bcd) };

    const u64 epoch = to_epoch(time);
    if (epoch == 0) { LOG_WARN("rtc: the clock reads nonsense; time starts at the epoch"); return; }
    clock::set_realtime(epoch * 1'000'000'000);
    LOG_INFO("rtc: %04u-%02u-%02u %02u:%02u:%02u", time.year, time.month, time.day, time.hour, time.minute, time.second);
}

} // namespace rtc

#endif // SIRIUS_HOST_TEST
